#include "bot_empires.h"
#include "bot_empires_capture_route_terminal.h"
#include "bot_empires_telemetry.h"
#include "bot_globals.h"
#include "bot_buttons.h"
#include "bot_getprop.h"
#include "in_buttons.h"
#include "bot_mods.h"
#include "bot_navigator.h"
#include "bot_navmesh_api.h"
#include "bot_navmesh_provider.h"
#include "bot_schedule.h"
#include "bot_task.h"
#include "bot_waypoint.h"
#include "bot_waypoint_locations.h"
#include "bot_weapons.h"
#include "rcbot/logging.h"
#include "engine/IEngineTrace.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <vector>
#include <cstdio>
#include <string>

int EmpiresVehiclePlayerVehicle(const CBotEmpires* bot, const edict_t* player);

namespace
{
constexpr float kEmpiresCaptureRejectedCooldown = 30.0f;
}

void CBotEmpires::beginEmpiresCaptureRouteAttempt()
{
	// Diagnostic-only identity: route issuance already exists at every call
	// site below; this makes each issuance correlatable without changing the
	// schedule, retry budget, or objective ownership.
	++m_iEmpiresCaptureRouteAttemptGeneration;
	if (m_iEmpiresCaptureRouteAttemptGeneration == 0)
		++m_iEmpiresCaptureRouteAttemptGeneration;
	m_flEmpiresCaptureRouteIssuedTime = engine != nullptr ?
		engine->Time() : -1.0f;
}

bool CBotEmpires::requestEmpiresCaptureApproach()
{
{
	if (m_pSchedules == nullptr || !m_pSchedules->isEmpty() ||
		engine == nullptr || gpGlobals == nullptr || m_pController == nullptr)
		return false;

	CBotMod* pCurrentMod = CBotGlobals::getCurrentMod();
	if (pCurrentMod == nullptr || pCurrentMod->getBotType() != BOTTYPE_EMPIRES)
		return false;

	if (m_bEmpiresCaptureApproachIssued)
	{
		const Vector currentOrigin = getOrigin();
		const float remainingDistance = distanceFrom(m_vEmpiresCaptureApproachOrigin);
		if (remainingDistance <= 160.0f)
		{
			const float now = engine->Time();
			if (!m_bEmpiresCaptureHoldIssued)
			{
				m_bEmpiresCaptureHoldIssued = true;
				m_flEmpiresCaptureHoldDeadline = now + 60.0f;
				// Arrival moots any past combat interruption.
				m_captureInterruptionLatch.reset();
				// Arrival also moots an active recovery leg: reaching the
				// capture area proves the bot re-entered navigable space.
				if (m_empiresStartAnchorRecovery.active)
				{
					recordEmpiresStartAnchorRecoveryEvent(
						"START_ANCHOR_RECOVERY_SUCCEEDED", "arrival",
						m_vEmpiresStartAnchorRecoveryStartOrigin,
						m_flEmpiresStartAnchorRecoveryStartTime,
						m_iEmpiresStartAnchorRecoveryWaypoint);
					m_empiresStartAnchorRecovery.finish();
				}
				observeEmpiresRecoveryProgress("capture-hold-participation");
			}

			if (now < m_flEmpiresCaptureHoldDeadline)
			{
				reuseEmpiresRecoveryEpisode("capture-hold-schedule");
				CBotSchedule* pCaptureHoldSchedule = new CBotSchedule();
				pCaptureHoldSchedule->addTask(new CBotWaitTask(
					m_flEmpiresCaptureHoldDeadline - now));
				m_pSchedules->add(pCaptureHoldSchedule);
				logger->Log(LogLevel::INFO,
					"Empires gameplay objective=CAPTURE_HOLD started point=%d owner_userid=%d team=%d capture_generation=%llu capture_attempt=%llu curtime=%.3f remaining_distance=%.1f hold_seconds=%.1f reason=wait-for-natural-ownership",
					m_iEmpiresCaptureApproachPoint,
					engine->GetPlayerUserId(m_pEdict), getTeam(),
					static_cast<unsigned long long>(m_iEmpiresCaptureObjectiveGeneration),
					static_cast<unsigned long long>(m_iEmpiresCaptureRouteAttemptGeneration),
					gpGlobals != nullptr ? gpGlobals->curtime : 0.0f,
					remainingDistance,
					m_flEmpiresCaptureHoldDeadline - now);
				const std::string telemetryDetail =
					"point=" + std::to_string(m_iEmpiresCaptureApproachPoint) +
					";" + getEmpiresCombatCorrelation();
				EmpiresAnalysis::RecordBotEvent(this, "CAPTURE_HOLD", nullptr,
					telemetryDetail.c_str());
				return true;
			}
		}

		// The latch is one-shot: take it once per drained schedule and share
		// the value with the recovery, retry, and reject paths below so a
		// combat interruption is attributed exactly once.
		const bool combatLatchBefore =
			m_captureInterruptionLatch.interruptedByCombat;
		const int retryBefore = m_iEmpiresCaptureApproachRetries;
		const bool objectiveValidBefore =
			isEmpiresCaptureApproachPointStillValid();
		const bool captureDrainCombatResume =
			m_captureInterruptionLatch.takeCombatInterruption();
		// A drained start-anchor recovery leg either restored anchoring
		// (normal route re-issued inside) or failed (existing accounting
		// below takes over with the shared latch value).
		if (remainingDistance > 160.0f && m_empiresStartAnchorRecovery.active &&
			tryFinishEmpiresStartAnchorRecovery(captureDrainCombatResume))
		{
			return true;
		}
		// A normal capture route can finish its schedule while the bot is still
		// far from the live capture origin. Reuse the same existing A*/MoveTo
		// route a small bounded number of times before allowing objective
		// arbitration to select again. This prevents schedule completion from
		// immediately selecting the same distant point in a tight loop without
		// adding a second planner or writing objective state.
		if (remainingDistance > 160.0f &&
			m_iEmpiresCaptureRouteWaypoint >= 0 &&
			m_iEmpiresCaptureApproachRetries <
				RCBotEmpiresCaptureRetry::kMaxCaptureRouteRetries)
		{
		// A combat interruption is not a route failure: resume the still
		// valid capture without spending retry budget. The marker is
		// one-shot; any later genuine stall consumes normally, and an
		// invalidated objective consumes exactly as before.
		const bool combatResume = captureDrainCombatResume;
		// A NO_START_ANCHOR zero-progress drain fails before any useful
		// route exists: re-issuing the identical route from the same
		// stationary position cannot succeed. Start one bounded re-anchor
		// leg instead, without touching the route retry budget.
		if (tryStartEmpiresStartAnchorRecovery(combatResume))
		{
			return true;
		}
			const bool consumeRetry =
				RCBotEmpiresCaptureRetry::ShouldConsumeCaptureRouteRetry(
					combatResume,
					combatResume ? objectiveValidBefore :
						true);
			if (consumeRetry)
			{
				++m_iEmpiresCaptureApproachRetries;
			}
		reuseEmpiresRecoveryEpisode("capture-alternate-approach");
		CBotSchedule* pRetrySchedule = new CBotSchedule();
		pRetrySchedule->addTask(new CFindPathTask(
			m_iEmpiresCaptureRouteWaypoint, LOOK_WAYPOINT));
		pRetrySchedule->addTask(new CMoveToTask(
			m_vEmpiresCaptureApproachOrigin));
		m_pSchedules->add(pRetrySchedule);
			m_flEmpiresNextCaptureApproachProgressLogTime =
				gpGlobals->curtime + 3.0f;
			if (consumeRetry)
			{
				logger->Log(LogLevel::INFO,
					"Empires gameplay objective=CAPTURE_APPROACH retry point=%d owner_userid=%d retry=%d max_retries=2 remaining_distance=%.1f route_waypoint=%d reason=schedule-complete-before-arrival",
					m_iEmpiresCaptureApproachPoint,
					engine->GetPlayerUserId(m_pEdict),
					m_iEmpiresCaptureApproachRetries,
					remainingDistance, m_iEmpiresCaptureRouteWaypoint);
				const std::string telemetryDetail =
					"objective=capture;point=" +
					std::to_string(m_iEmpiresCaptureApproachPoint) +
					";route_waypoint=" +
					std::to_string(m_iEmpiresCaptureRouteWaypoint);
				EmpiresAnalysis::RecordBotEvent(this, "RETRY", nullptr,
					telemetryDetail.c_str());
		}
		// Diagnostic-only terminal event for this drain. Read-only. The
		// route-attempt timestamp updates after emission so this drain's
		// terminal (recorded before the re-issue) passes the attribution
		// gate; terminals from the new attempt gate against the new time.
		emitEmpiresCaptureRouteTerminal(combatResume, consumeRetry,
			consumeRetry ? "retry-consume" : "retry-resume",
			combatLatchBefore, objectiveValidBefore, retryBefore,
			m_iEmpiresCaptureRouteAttemptGeneration + 1);
		// Diagnostic-only route-attempt timestamp for terminal attribution.
		const std::uint64_t previousAttemptGeneration =
			m_iEmpiresCaptureRouteAttemptGeneration;
		beginEmpiresCaptureRouteAttempt();
		const char* reissueDecision = consumeRetry ? "RETRY" : "RESUME";
		const std::string reissueTelemetryDetail =
			"same_objective=1;previous_attempt_generation=" +
			std::to_string(previousAttemptGeneration) + ";decision=" +
			reissueDecision + ";" + getEmpiresCombatCorrelation();
		EmpiresAnalysis::RecordBotEvent(this,
			consumeRetry ? "CAPTURE_APPROACH_REISSUED" :
				"CAPTURE_APPROACH_RESUMED", nullptr,
			reissueTelemetryDetail.c_str());
		logger->Log(LogLevel::INFO,
			"Empires gameplay objective=CAPTURE_APPROACH %s point=%d owner_userid=%d team=%d capture_generation=%llu capture_attempt=%llu previous_attempt_generation=%llu curtime=%.3f same_objective=1 combat_resume=%d consume_retry=%d reason=schedule-complete-before-arrival",
			consumeRetry ? "reissued" : "resumed",
			m_iEmpiresCaptureApproachPoint,
			engine->GetPlayerUserId(m_pEdict), getTeam(),
			static_cast<unsigned long long>(m_iEmpiresCaptureObjectiveGeneration),
			static_cast<unsigned long long>(m_iEmpiresCaptureRouteAttemptGeneration),
			static_cast<unsigned long long>(previousAttemptGeneration),
			gpGlobals != nullptr ? gpGlobals->curtime : 0.0f,
			combatResume ? 1 : 0, consumeRetry ? 1 : 0);
		return true;
	}

		if (remainingDistance > 160.0f && m_iEmpiresCaptureApproachPoint >= 0)
		{
			// Diagnostic-only: the route drained before arrival and the retry
			// budget is exhausted (or the point is gone). Read-only event.
			// The latch was already taken once above; reuse the shared value.
			emitEmpiresCaptureRouteTerminal(captureDrainCombatResume, true,
				"reject", combatLatchBefore, objectiveValidBefore,
				retryBefore, 0);
			m_iEmpiresCaptureRejectedPoint = m_iEmpiresCaptureApproachPoint;
			m_flEmpiresCaptureRejectedUntil = engine->Time() +
				kEmpiresCaptureRejectedCooldown;
			logger->Log(LogLevel::INFO,
				"Empires gameplay objective=CAPTURE_APPROACH route-rejected point=%d owner_userid=%d remaining_distance=%.1f cooldown=%.1f reason=bounded-route-exhausted",
				m_iEmpiresCaptureRejectedPoint,
				engine->GetPlayerUserId(m_pEdict), remainingDistance,
				kEmpiresCaptureRejectedCooldown);
			const std::string telemetryDetail =
				"capture-route-rejected;point=" +
				std::to_string(m_iEmpiresCaptureRejectedPoint);
			EmpiresAnalysis::RecordBotEvent(this, "OBJECTIVE_FAILED", nullptr,
				telemetryDetail.c_str());
			EmpiresAnalysis::CorrelateNavigationSignal(this,
				RCBotNavigationExperience::Signal::CAPTURE_ROUTE_EXHAUSTED);
			observeEmpiresRecoveryDomainExhausted(
				"capture-route-recovery-exhausted");
		}

		logger->Log(LogLevel::INFO,
			"Empires gameplay objective=CAPTURE_APPROACH ended point=%d owner_userid=%d reason=schedule-complete current_origin=(%.1f %.1f %.1f) remaining_distance=%.1f",
			m_iEmpiresCaptureApproachPoint,
			engine->GetPlayerUserId(m_pEdict), currentOrigin.x, currentOrigin.y,
			currentOrigin.z, remainingDistance);
		m_bEmpiresCaptureApproachIssued = false;
		m_bEmpiresCaptureHoldIssued = false;
		m_iEmpiresCaptureApproachPoint = -1;
		m_iEmpiresCaptureRouteWaypoint = -1;
		m_iEmpiresCaptureApproachRetries = 0;
		m_captureInterruptionLatch.reset();
		clearEmpiresStartAnchorRecovery();
	m_flEmpiresCaptureRouteIssuedTime = -1.0f;
		m_vEmpiresCaptureApproachStartOrigin = vec3_origin;
		m_flEmpiresNextCaptureApproachProgressLogTime = 0.0f;
		m_flEmpiresCaptureHoldDeadline = 0.0f;
	}

	if (engine->Time() < m_flEmpiresNextCaptureApproachTime)
		return false;

	const int team = getTeam();
	if (team != EMP_TEAM_NF && team != EMP_TEAM_BE)
		return false;

	const CEmpiresMod* pEmpiresMod = static_cast<const CEmpiresMod*>(pCurrentMod);
	bool linearMapOrderingActive = !pEmpiresMod->getCapturePoints().empty();
	for (const CEmpiresMod::CapturePointObservation& point :
		pEmpiresMod->getCapturePoints())
	{
		if (!point.pointIndexKnown || !point.linearMapKnown || !point.linearMap)
		{
			linearMapOrderingActive = false;
			break;
		}
	}
	const CEmpiresMod::CapturePointObservation* pBest = nullptr;
	int bestRouteWaypoint = -1;
	Vector bestRouteWaypointOrigin = vec3_origin;
	Vector bestFinalOrigin = vec3_origin;
	bool bestUsesNearestWaypoint = false;
	float bestDistance = INFINITE_DISTANCE;
	for (const CEmpiresMod::CapturePointObservation& point :
		pEmpiresMod->getCapturePoints())
	{
		if (!point.pointIndexKnown || !point.originKnown)
			continue;
		if (isEmpiresCaptureCandidateSuppressed(point.pointIndex))
			continue;
		if (point.pointIndex == m_iEmpiresCaptureRejectedPoint &&
			engine->Time() < m_flEmpiresCaptureRejectedUntil)
		{
			logger->Log(LogLevel::INFO,
				"Empires gameplay objective=CAPTURE_APPROACH route-skipped point=%d owner_userid=%d cooldown_remaining=%.1f reason=route-failure-cooldown",
				point.pointIndex, engine->GetPlayerUserId(m_pEdict),
				m_flEmpiresCaptureRejectedUntil - engine->Time());
			continue;
		}

		const int ownStatus = team == EMP_TEAM_NF ? point.nfStatus : point.beStatus;
		const bool ownStatusKnown = team == EMP_TEAM_NF ?
			point.nfStatusKnown : point.beStatusKnown;
		const int opposingStatus = team == EMP_TEAM_NF ? point.beStatus : point.nfStatus;
		const bool opposingStatusKnown = team == EMP_TEAM_NF ?
			point.beStatusKnown : point.nfStatusKnown;
		const int teamCappable = team == EMP_TEAM_NF ?
			point.nfCappable : point.beCappable;
		const bool teamCappableKnown = team == EMP_TEAM_NF ?
			point.nfCappableKnown : point.beCappableKnown;
		if (teamCappableKnown && teamCappable == 0)
			continue;
		// Prefer the authoritative owner property over the UI status fields.
		// A point is a valid current/front-line target only when Empires marks it
		// cappable for this team and it is neutral or owned by the opposing team.
		// This prevents selecting an already-owned or behind-front point merely
		// because its entity remains present in the map.
		if (point.teamOwnerKnown)
		{
			const int opposingTeam = team == EMP_TEAM_NF ? EMP_TEAM_BE : EMP_TEAM_NF;
			if (point.teamOwner != 1 && point.teamOwner != team &&
				point.teamOwner != opposingTeam)
				continue;
			if (point.teamOwner == team)
				continue;
		}
		else if (!ownStatusKnown ||
			(opposingStatusKnown && ownStatus == 1 && opposingStatus == 0))
		{
			continue;
		}

		int routeWaypoint = -1;
		Vector routeWaypointOrigin = vec3_origin;
		Vector finalOrigin = point.origin;
		bool usesNearestWaypoint = false;
		if (point.captureWaypointKnown && point.captureWaypoint >= 0)
		{
			routeWaypoint = point.captureWaypoint;
			routeWaypointOrigin = point.captureWaypointOrigin;
			// The waypoint is already inside the live capture brush.  Do not
			// leave the navigable volume by steering toward the brush center.
			finalOrigin = point.captureWaypointOrigin;
		}
		else if (point.waypointAssociationKnown && point.nearestWaypoint >= 0)
		{
			CWaypoint* pNearestWaypoint = CWaypoints::getWaypoint(point.nearestWaypoint);
			if (pNearestWaypoint == nullptr)
				continue;
			routeWaypoint = point.nearestWaypoint;
			routeWaypointOrigin = pNearestWaypoint->getOrigin();
			usesNearestWaypoint = true;
			if (point.groundApproachKnown)
				finalOrigin = point.groundApproachOrigin;
		}
		else
		{
			continue;
		}

		const float distance = distanceFrom(finalOrigin);
		bool betterCandidate = pBest == nullptr || distance < bestDistance;
		if (linearMapOrderingActive && pBest != nullptr)
		{
			const bool advancesLinearFront = team == EMP_TEAM_NF ?
				point.pointIndex < pBest->pointIndex :
				point.pointIndex > pBest->pointIndex;
			betterCandidate = advancesLinearFront ||
				(point.pointIndex == pBest->pointIndex && distance < bestDistance);
		}
		if (betterCandidate)
		{
			pBest = &point;
			bestRouteWaypoint = routeWaypoint;
			bestRouteWaypointOrigin = routeWaypointOrigin;
			bestFinalOrigin = finalOrigin;
			bestUsesNearestWaypoint = usesNearestWaypoint;
			bestDistance = distance;
		}
	}

	if (pBest == nullptr)
		return false;

	const std::string captureSubject =
		std::to_string(pBest->pointIndex);
	const RCBotRecovery::RecoveryEpisodeIdentity& currentEpisode =
		getRecoveryEpisodeIdentity();
	const bool sameCaptureObjective = currentEpisode.IsActive() &&
		currentEpisode.domain == RCBotRecovery::Domain::CAPTURE &&
		currentEpisode.subjectId == captureSubject;
	if (!sameCaptureObjective)
	{
		++m_iEmpiresCaptureObjectiveGeneration;
		if (m_iEmpiresCaptureObjectiveGeneration == 0)
			++m_iEmpiresCaptureObjectiveGeneration;
	}
	const std::uint64_t captureGeneration = sameCaptureObjective ?
		currentEpisode.domainGeneration : m_iEmpiresCaptureObjectiveGeneration;
	ensureEmpiresRecoveryEpisode(RCBotRecovery::Domain::CAPTURE,
		captureSubject.c_str(), captureGeneration, "capture-objective-selected");

	// Prefer a waypoint inside the capture volume.  If the current capture
	// point has no such waypoint, use the nearest graph waypoint as the route
	// anchor and keep the actual capture-volume origin as the final direct leg.
	// This preserves the existing RCBot2 pathing while allowing the bot to
	// enter valid capture brushes that are not themselves waypointed.
	CBotSchedule* pCaptureSchedule = new CBotSchedule();
	pCaptureSchedule->addTask(new CFindPathTask(bestRouteWaypoint, LOOK_WAYPOINT));
	CMoveToTask* pCaptureApproach = new CMoveToTask(bestFinalOrigin);
	if (bestUsesNearestWaypoint)
	{
		// The final leg starts at a valid graph waypoint but may need to clear
		// the waypoint's collision geometry before entering the capture brush.
		// Do not let a transient Euclidean-distance increase complete the
		// fallback route before the bot reaches the live capture origin.
		pCaptureApproach->ignoreDistanceIncrease();
	}
	pCaptureSchedule->addTask(pCaptureApproach);
	m_pSchedules->add(pCaptureSchedule);
	m_bEmpiresCaptureApproachIssued = true;
	m_bEmpiresCaptureHoldIssued = false;
	m_iEmpiresCaptureApproachPoint = pBest->pointIndex;
	m_iEmpiresRecoveryCapturePoint = pBest->pointIndex;
	m_iEmpiresCaptureRouteWaypoint = bestRouteWaypoint;
	m_iEmpiresCaptureApproachRetries = 0;
	m_captureInterruptionLatch.reset();
	clearEmpiresStartAnchorRecovery();
	beginEmpiresCaptureRouteAttempt();
	m_vEmpiresCaptureApproachOrigin = bestFinalOrigin;
	m_vEmpiresCaptureApproachStartOrigin = getOrigin();
	m_flEmpiresNextCaptureApproachTime = engine->Time() + 15.0f;
	m_flEmpiresNextCaptureApproachProgressLogTime = gpGlobals->curtime + 3.0f;
	m_flEmpiresCaptureHoldDeadline = 0.0f;
	const std::string telemetryDetail =
		"capture-point=" + std::to_string(pBest->pointIndex) +
		";route_waypoint=" + std::to_string(bestRouteWaypoint) +
		";" + getEmpiresCombatCorrelation();
	EmpiresAnalysis::RecordBotEvent(this, "OBJECTIVE_SELECTED", nullptr,
		telemetryDetail.c_str());
	EmpiresAnalysis::RecordBotEvent(this, "CAPTURE_STARTED", nullptr,
		telemetryDetail.c_str());
	logger->Log(LogLevel::INFO,
		"Empires gameplay objective=CAPTURE_APPROACH started owner_userid=%d team=%d point=%d capture_generation=%llu capture_attempt=%llu curtime=%.3f own_status=%d team_owner=%d team_owner_known=%d team_cappable=%d team_cappable_known=%d capture_waypoint=%d capture_waypoint_origin=(%.1f %.1f %.1f) distance=%.1f route=CBot2_CFindPathTask(capture-waypoint)+CBot2_CMoveToTask(capture-waypoint-origin) action=physical-capture-deferred route_mode=%s selection_mode=%s target_origin=(%.1f %.1f %.1f)",
		engine->GetPlayerUserId(m_pEdict), team, pBest->pointIndex,
		static_cast<unsigned long long>(m_iEmpiresCaptureObjectiveGeneration),
		static_cast<unsigned long long>(m_iEmpiresCaptureRouteAttemptGeneration),
		gpGlobals->curtime,
		team == EMP_TEAM_NF ? pBest->nfStatus : pBest->beStatus,
		pBest->teamOwner, pBest->teamOwnerKnown ? 1 : 0,
		team == EMP_TEAM_NF ? pBest->nfCappable : pBest->beCappable,
		team == EMP_TEAM_NF ? pBest->nfCappableKnown : pBest->beCappableKnown,
		bestRouteWaypoint,
		bestUsesNearestWaypoint ? bestRouteWaypointOrigin.x : bestFinalOrigin.x,
		bestUsesNearestWaypoint ? bestRouteWaypointOrigin.y : bestFinalOrigin.y,
		bestUsesNearestWaypoint ? bestRouteWaypointOrigin.z : bestFinalOrigin.z,
		bestDistance,
		bestUsesNearestWaypoint ? "nearest-waypoint-plus-capture-origin" : "capture-waypoint",
		linearMapOrderingActive ? "linear-point-order" : "nearest-route-distance",
		bestFinalOrigin.x, bestFinalOrigin.y, bestFinalOrigin.z);
	return true;
}

}

void CBotEmpires::emitEmpiresCaptureRouteTerminal(const bool combatResume,
	const bool consumeRetry, const char* decision,
	const bool combatLatchBefore, const bool objectiveValidBefore,
	const int retryBefore, const std::uint64_t nextAttemptGeneration)
{
	// Diagnostic-only read of already-recorded state. Must not change retry,
	// latch, navigation, movement, stuck, recovery, or cooldown state.
	const Vector currentOrigin = getOrigin();
	const float remainingDistance = distanceFrom(m_vEmpiresCaptureApproachOrigin);
	const float displacementSinceIssue =
		(currentOrigin - m_vEmpiresCaptureApproachStartOrigin).Length();

	// Attribute the per-bot task terminal only when it was recorded since
	// the current route attempt was issued. Older terminals (including any
	// combat-task terminal when combatResume is set) must not be mistaken
	// for this route's drain cause: combat preemption is attributed via the
	// latch (combatResume), which takes precedence over the slot.
	const TaskTerminalNote terminalNote = lastTaskTerminal();
	const bool terminalFresh =
		m_flEmpiresCaptureRouteIssuedTime >= 0.0f &&
		terminalNote.time >= m_flEmpiresCaptureRouteIssuedTime;
	const TaskTerminalCause rawCause = terminalFresh ?
		terminalNote.cause : TaskTerminalCause::UNKNOWN;
	const int terminalTaskWaypoint = terminalFresh ? terminalNote.waypoint : -1;
	namespace CaptureTerminal = RCBotEmpiresCaptureRouteTerminal;
	const CaptureTerminal::RouteTerminal routeTerminal =
		combatResume ? CaptureTerminal::RouteTerminal::UNKNOWN :
		CaptureTerminal::MapTaskTerminalCause(rawCause);
	const CaptureTerminal::ZeroProgressCause suggestedCause =
		combatResume ? CaptureTerminal::ZeroProgressCause::H_OTHER :
		CaptureTerminal::SuggestCauseForTerminal(routeTerminal);

	const char* providerName = "intent-inactive";
	if (hasActiveNavigationIntent())
	{
		switch (getNavigationIntent().provider)
		{
		case RCBotNavigation::Provider::WAYPOINT:
			providerName = "WAYPOINT";
			break;
		case RCBotNavigation::Provider::NAVMESH:
			providerName = "NAVMESH";
			break;
		case RCBotNavigation::Provider::LOCAL_EXPLORER:
			providerName = "LOCAL_EXPLORER";
			break;
		case RCBotNavigation::Provider::BUILDING_RELATIVE:
			providerName = "BUILDING_RELATIVE";
			break;
		case RCBotNavigation::Provider::UNKNOWN:
		default:
			providerName = "UNKNOWN";
			break;
		}
	}

	const bool stuckLatched = hasLatchedMoveFailure();
	const float stuckDeadlineIn =
		m_fWaypointStuckTime > 0.0f ?
		m_fWaypointStuckTime - engine->Time() : 0.0f;
	const RCBotRecovery::RecoveryEpisodeIdentity& recoveryIdentity =
		getRecoveryEpisodeIdentity();
	const bool recoveryActive = recoveryIdentity.IsActive();
	const int retryBeforeValue = retryBefore >= 0 ? retryBefore :
		m_iEmpiresCaptureApproachRetries;

	logger->Log(LogLevel::INFO,
		"Empires gameplay objective=CAPTURE_ROUTE_TERMINAL point=%d owner_userid=%d owner_team=%d capture_generation=%llu capture_attempt=%llu curtime=%.3f retry=%d retry_before=%d retry_after=%d decision=%s combat_latch_before=%d capture_valid_before=%d combat_resume=%d consume_retry=%d next_attempt_generation=%llu remaining_distance=%.1f displacement_since_issue=%.1f route_waypoint=%d terminal_task_waypoint=%d task_cause=%s route_terminal=%s suggested=%s provider=%s stuck_latched=%d stuck_deadline_in=%.1f recovery_active=%d recovery_domain=%s",
		m_iEmpiresCaptureApproachPoint,
		engine->GetPlayerUserId(m_pEdict),
		getTeam(),
		static_cast<unsigned long long>(m_iEmpiresCaptureObjectiveGeneration),
		static_cast<unsigned long long>(m_iEmpiresCaptureRouteAttemptGeneration),
		gpGlobals != nullptr ? gpGlobals->curtime : 0.0f,
		m_iEmpiresCaptureApproachRetries, retryBeforeValue,
		m_iEmpiresCaptureApproachRetries, decision,
		combatLatchBefore ? 1 : 0, objectiveValidBefore ? 1 : 0,
		combatResume ? 1 : 0, consumeRetry ? 1 : 0,
		static_cast<unsigned long long>(nextAttemptGeneration),
		remainingDistance, displacementSinceIssue,
		m_iEmpiresCaptureRouteWaypoint, terminalTaskWaypoint,
		TaskTerminalCauseName(rawCause),
		CaptureTerminal::RouteTerminalName(routeTerminal),
		CaptureTerminal::ZeroProgressCauseName(suggestedCause),
		providerName, stuckLatched ? 1 : 0, stuckDeadlineIn,
		recoveryActive ? 1 : 0,
		RCBotRecovery::DomainName(recoveryIdentity.domain));
	const std::string detail =
		"capture-route-terminal;point=" +
		std::to_string(m_iEmpiresCaptureApproachPoint) +
		";retry=" + std::to_string(m_iEmpiresCaptureApproachRetries) +
		";decision=" + (decision != nullptr ? decision : "unknown") +
		";combat_resume=" + std::to_string(combatResume ? 1 : 0) +
		";consume_retry=" + std::to_string(consumeRetry ? 1 : 0) +
		";remaining=" + std::to_string(remainingDistance) +
		";displacement=" + std::to_string(displacementSinceIssue) +
		";route_waypoint=" + std::to_string(m_iEmpiresCaptureRouteWaypoint) +
		";task_waypoint=" + std::to_string(terminalTaskWaypoint) +
		";task_cause=" + TaskTerminalCauseName(rawCause) +
		";route_terminal=" + CaptureTerminal::RouteTerminalName(routeTerminal) +
		";suggested=" + CaptureTerminal::ZeroProgressCauseName(suggestedCause) +
		";provider=" + providerName +
		";stuck=" + std::to_string(stuckLatched ? 1 : 0) +
		";recovery=" + std::to_string(recoveryActive ? 1 : 0) + ";" +
		RCBotEmpiresCombatTelemetry::FormatRetryAccounting(
			combatLatchBefore, objectiveValidBefore, retryBeforeValue,
			m_iEmpiresCaptureApproachRetries, combatResume, consumeRetry) +
		";attempt_generation=" +
		std::to_string(m_iEmpiresCaptureRouteAttemptGeneration) +
		";next_attempt_generation=" +
		std::to_string(nextAttemptGeneration) + ";" +
		getEmpiresCombatCorrelation();
	EmpiresAnalysis::RecordBotEvent(this, "CAPTURE_ROUTE_TERMINAL", nullptr,
		detail.c_str());
}

RouteAnchorStatus CBotEmpires::probeEmpiresCaptureRouteAnchors()
{
	if (m_pNavigator == nullptr || m_iEmpiresCaptureRouteWaypoint < 0)
		return RouteAnchorStatus::READY;
	return m_pNavigator->prepareRouteAnchors(getOrigin(),
		m_vEmpiresCaptureApproachOrigin, m_iEmpiresCaptureRouteWaypoint);
}

void CBotEmpires::recordEmpiresStartAnchorRecoveryEvent(const char* eventName,
	const char* reason, const Vector& startOrigin, const float startTime,
	const int reentryWaypoint)
{
	namespace StartRecovery = RCBotEmpiresStartAnchorRecovery;
	const Vector endOrigin = getOrigin();
	const float displacement = (endOrigin - startOrigin).Length();
	const float duration = startTime >= 0.0f && engine != nullptr ?
		engine->Time() - startTime : 0.0f;
	const RouteAnchorStatus anchorStatus = probeEmpiresCaptureRouteAnchors();
	logger->Log(LogLevel::INFO,
		"Empires gameplay %s point=%d owner_userid=%d attempt=%d reason=%s start=(%.1f %.1f %.1f) end=(%.1f %.1f %.1f) displacement=%.1f duration=%.1f reentry_waypoint=%d anchor=%s",
		eventName, m_iEmpiresCaptureApproachPoint,
		engine != nullptr ? engine->GetPlayerUserId(m_pEdict) : -1,
		m_empiresStartAnchorRecovery.attemptsUsedFor(
			m_iEmpiresCaptureApproachPoint),
		reason != nullptr ? reason : "unspecified",
		startOrigin.x, startOrigin.y, startOrigin.z,
		endOrigin.x, endOrigin.y, endOrigin.z,
		displacement, duration, reentryWaypoint,
		routeAnchorStatusName(anchorStatus));
	char detail[512];
	std::snprintf(detail, sizeof(detail),
		"start-anchor-recovery;event=%s;point=%d;userid=%d;attempt=%d;reason=%s;start=(%.1f %.1f %.1f);end=(%.1f %.1f %.1f);displacement=%.1f;duration=%.1f;reentry_waypoint=%d;anchor=%s",
		eventName, m_iEmpiresCaptureApproachPoint,
		engine != nullptr ? engine->GetPlayerUserId(m_pEdict) : -1,
		m_empiresStartAnchorRecovery.attemptsUsedFor(
			m_iEmpiresCaptureApproachPoint),
		reason != nullptr ? reason : "unspecified",
		startOrigin.x, startOrigin.y, startOrigin.z,
		endOrigin.x, endOrigin.y, endOrigin.z,
		displacement, duration, reentryWaypoint,
		routeAnchorStatusName(anchorStatus));
	EmpiresAnalysis::RecordBotEvent(this, eventName, nullptr, detail);
}

bool CBotEmpires::tryStartEmpiresStartAnchorRecovery(const bool combatResume)
{
	namespace StartRecovery = RCBotEmpiresStartAnchorRecovery;
	if (combatResume || m_iEmpiresCaptureApproachPoint < 0 ||
		m_iEmpiresCaptureRouteWaypoint < 0 || m_pNavigator == nullptr ||
		engine == nullptr)
	{
		return false;
	}
	// Attribute the per-bot task terminal exactly like the terminal event:
	// only a terminal recorded since the current route attempt was issued
	// may gate this decision.
	const TaskTerminalNote terminalNote = lastTaskTerminal();
	const bool anchorNotReady = m_flEmpiresCaptureRouteIssuedTime >= 0.0f &&
		terminalNote.time >= m_flEmpiresCaptureRouteIssuedTime &&
		terminalNote.cause == TaskTerminalCause::FIND_PATH_ANCHOR_NOT_READY;
	if (!anchorNotReady)
		return false;
	// Read-only re-probe at drain time: only a start-side failure (the
	// bot's own position has no usable anchor) triggers recovery.
	// Destination-side failures keep existing accounting.
	if (probeEmpiresCaptureRouteAnchors() !=
		RouteAnchorStatus::NO_START_ANCHOR)
	{
		return false;
	}
	if (!isEmpiresCaptureApproachPointStillValid())
		return false;
	const Vector currentOrigin = getOrigin();
	const float displacementSinceIssue =
		(currentOrigin - m_vEmpiresCaptureApproachStartOrigin).Length();
	if (StartRecovery::DecideCaptureDrainAction(false, true, true, true,
		displacementSinceIssue,
		m_empiresStartAnchorRecovery.attemptsUsedFor(
			m_iEmpiresCaptureApproachPoint)) !=
		StartRecovery::DrainAction::START_RECOVERY)
	{
		return false;
	}
	// Re-entry target: the nearest team-usable graph waypoint. Walking its
	// direct leg moves the bot back toward graph coverage so normal route
	// anchoring can work again. The destination capture waypoint is never
	// excluded or replaced.
	const int reentryWaypoint = CWaypointLocations::NearestWaypoint(
		currentOrigin, StartRecovery::kMaxStartAnchorReentryDistance, -1,
		false, false, true, nullptr, false, getTeam(), false);
	CWaypoint* pReentryWaypoint = reentryWaypoint >= 0 ?
		CWaypoints::getWaypoint(reentryWaypoint) : nullptr;
	if (pReentryWaypoint == nullptr)
		return false;
	m_empiresStartAnchorRecovery.begin(m_iEmpiresCaptureApproachPoint);
	m_iEmpiresStartAnchorRecoveryWaypoint = reentryWaypoint;
	m_vEmpiresStartAnchorRecoveryStartOrigin = currentOrigin;
	m_flEmpiresStartAnchorRecoveryStartTime = engine->Time();
	recordEmpiresStartAnchorRecoveryEvent("START_ANCHOR_RECOVERY_STARTED",
		"no-start-anchor", currentOrigin,
		m_flEmpiresStartAnchorRecoveryStartTime, reentryWaypoint);
	// Reuse the existing dormant direct-fallback pairing: the armed
	// CFindPathTask completes via FIND_PATH_DIRECT_FALLBACK when its
	// preflight still finds no start anchor, leaving the trailing
	// CMoveToTask alive as the bounded direct re-entry leg (arrival,
	// stuck-sentinel, and no-progress guards unchanged).
	reuseEmpiresRecoveryEpisode("capture-start-anchor-recovery");
	CBotSchedule* pRecoverySchedule = new CBotSchedule();
	CFindPathTask* pReanchorTask =
		new CFindPathTask(reentryWaypoint, LOOK_WAYPOINT);
	pReanchorTask->fallbackToDirectOnFailure();
	pRecoverySchedule->addTask(pReanchorTask);
	CMoveToTask* pReentryLeg =
		new CMoveToTask(pReentryWaypoint->getOrigin());
	// The re-entry walk may skirt collision geometry around the waypoint;
	// do not let a transient Euclidean-distance increase complete the leg
	// before the bot reaches graph coverage (same rationale as the capture
	// final leg).
	pReentryLeg->ignoreDistanceIncrease();
	pRecoverySchedule->addTask(pReentryLeg);
	m_pSchedules->add(pRecoverySchedule);
	// Diagnostic-only terminal for this drain. Read-only; the route-attempt
	// timestamp updates after emission so the recovery leg's own terminal
	// attributes to the new attempt. No retry budget consumed: no useful
	// capture route existed to retry.
	emitEmpiresCaptureRouteTerminal(combatResume, false,
		"start-anchor-recovery");
	beginEmpiresCaptureRouteAttempt();
	m_flEmpiresNextCaptureApproachProgressLogTime = gpGlobals->curtime + 3.0f;
	return true;
}

bool CBotEmpires::tryFinishEmpiresStartAnchorRecovery(const bool combatResume)
{
	namespace StartRecovery = RCBotEmpiresStartAnchorRecovery;
	const int point = m_iEmpiresCaptureApproachPoint;
	const bool pointValid = isEmpiresCaptureApproachPointStillValid();
	const RouteAnchorStatus anchorStatus = probeEmpiresCaptureRouteAnchors();
	if (StartRecovery::ShouldReissueNormalRouteAfterRecovery(combatResume,
		pointValid, anchorStatus == RouteAnchorStatus::READY))
	{
		m_empiresStartAnchorRecovery.finish();
		recordEmpiresStartAnchorRecoveryEvent(
			"START_ANCHOR_RECOVERY_SUCCEEDED", "anchor-ready",
			m_vEmpiresStartAnchorRecoveryStartOrigin,
			m_flEmpiresStartAnchorRecoveryStartTime,
			m_iEmpiresStartAnchorRecoveryWaypoint);
		// Anchors resolve again: re-issue the normal capture route. This is
		// the first genuine route attempt for this approach (the failed
		// preflight never built one), so it consumes no retry budget.
		// Construction mirrors the existing retry schedule exactly.
		reuseEmpiresRecoveryEpisode(
			"capture-approach-after-start-anchor-recovery");
		CBotSchedule* pCaptureSchedule = new CBotSchedule();
		pCaptureSchedule->addTask(new CFindPathTask(
			m_iEmpiresCaptureRouteWaypoint, LOOK_WAYPOINT));
		pCaptureSchedule->addTask(new CMoveToTask(
			m_vEmpiresCaptureApproachOrigin));
		m_pSchedules->add(pCaptureSchedule);
		emitEmpiresCaptureRouteTerminal(combatResume, false,
			"recovery-reissue");
		beginEmpiresCaptureRouteAttempt();
		m_flEmpiresNextCaptureApproachProgressLogTime =
			gpGlobals->curtime + 3.0f;
		logger->Log(LogLevel::INFO,
			"Empires gameplay objective=CAPTURE_APPROACH recovery-reissue point=%d owner_userid=%d route_waypoint=%d reason=start-anchor-recovered",
			point, engine != nullptr ? engine->GetPlayerUserId(m_pEdict) : -1,
			m_iEmpiresCaptureRouteWaypoint);
		return true;
	}
	const char* reason = combatResume ? "combat-interrupted" :
		(!pointValid ? "objective-invalid" :
		routeAnchorStatusName(anchorStatus));
	recordEmpiresStartAnchorRecoveryEvent("START_ANCHOR_RECOVERY_FAILED",
		reason, m_vEmpiresStartAnchorRecoveryStartOrigin,
		m_flEmpiresStartAnchorRecoveryStartTime,
		m_iEmpiresStartAnchorRecoveryWaypoint);
	// Attempts stay consumed for this point: the next drain falls through
	// to existing bounded retry/reject accounting and can never re-enter
	// recovery for the same capture context.
	m_empiresStartAnchorRecovery.finish();
	return false;
}

bool CBotEmpires::isEmpiresCaptureApproachPointStillValid()
{
	if (m_iEmpiresCaptureApproachPoint < 0)
		return false;

	const int team = getTeam();
	if (team != EMP_TEAM_NF && team != EMP_TEAM_BE)
		return false;

	if (isEmpiresCaptureCandidateSuppressed(m_iEmpiresCaptureApproachPoint))
		return false;

	if (m_iEmpiresCaptureApproachPoint == m_iEmpiresCaptureRejectedPoint &&
		engine != nullptr && engine->Time() < m_flEmpiresCaptureRejectedUntil)
		return false;

	// Mirror the selection skip predicates: a held point that this team could
	// no longer select (uncappable, owned, or gone from observations) must
	// not be preserved merely because combat removed the schedule. An
	// unknown point likewise falls through to normal retry accounting.
	CBotMod* pCurrentMod = CBotGlobals::getCurrentMod();
	if (pCurrentMod == nullptr || pCurrentMod->getBotType() != BOTTYPE_EMPIRES)
		return false;
	const CEmpiresMod* pEmpiresMod =
		static_cast<const CEmpiresMod*>(pCurrentMod);
	for (const CEmpiresMod::CapturePointObservation& point :
		pEmpiresMod->getCapturePoints())
	{
		if (!point.pointIndexKnown ||
			point.pointIndex != m_iEmpiresCaptureApproachPoint)
			continue;
		if (team == EMP_TEAM_NF)
		{
			if (point.nfCappableKnown && point.nfCappable == 0)
				return false;
		}
		else if (point.beCappableKnown && point.beCappable == 0)
			return false;
		if (point.teamOwnerKnown)
		{
			const int opposingTeam = team == EMP_TEAM_NF ?
				EMP_TEAM_BE : EMP_TEAM_NF;
			if (point.teamOwner != 1 && point.teamOwner != team &&
				point.teamOwner != opposingTeam)
				return false;
			if (point.teamOwner == team)
				return false;
			return true;
		}
		const bool ownStatusKnown = team == EMP_TEAM_NF ?
			point.nfStatusKnown : point.beStatusKnown;
		const int ownStatus = team == EMP_TEAM_NF ?
			point.nfStatus : point.beStatus;
		const bool opposingStatusKnown = team == EMP_TEAM_NF ?
			point.beStatusKnown : point.nfStatusKnown;
		const int opposingStatus = team == EMP_TEAM_NF ?
			point.beStatus : point.nfStatus;
		if (!ownStatusKnown ||
			(opposingStatusKnown && ownStatus == 1 && opposingStatus == 0))
			return false;
		return true;
	}
	return false;
}

// ---------------------------------------------------------------------------
// Goal-Roam replacement for maps without waypoints.  Only used when
// CWaypoints::numWaypoints() == 0; targets come from map entities and are
// reached through the navmesh provider.  One target at a time: it is kept
// until arrival, failure or timeout, then the next one is chosen.  Failed
// targets are avoided for a while, and teammates are spread over the
// candidate list by their rank within the team.
// ---------------------------------------------------------------------------
namespace
{
	enum EmpiresNavGoalType
	{
		NAVGOAL_REPAIR = 0,   // Engineer: own damaged or unfinished building
		NAVGOAL_DEFEND,
		NAVGOAL_CAPTURE,
		NAVGOAL_RESOURCE,
		NAVGOAL_BUILDING,
		NAVGOAL_COMMANDER,
		NAVGOAL_TYPES
	};

	const char* empiresNavGoalTypeName(const int type)
	{
		switch (type)
		{
		case NAVGOAL_REPAIR: return "repair";
		case NAVGOAL_DEFEND: return "defend";
		case NAVGOAL_CAPTURE: return "capture";
		case NAVGOAL_RESOURCE: return "resource";
		case NAVGOAL_BUILDING: return "building";
		case NAVGOAL_COMMANDER: return "commander";
		default: return "none";
		}
	}

	float empiresNavGoalArriveRadius(const int type)
	{
		switch (type)
		{
		case NAVGOAL_REPAIR: return 40.0f;   // stand point next to the building
		case NAVGOAL_CAPTURE: return 150.0f;
		case NAVGOAL_DEFEND: return 250.0f;
		case NAVGOAL_RESOURCE: return 200.0f;
		default: return 300.0f; // building/commander origins sit inside solids
		}
	}

	constexpr float kNavGoalCaptureHold = 30.0f;   // game seconds on a cap point
	constexpr float kNavGoalArrivedAvoid = 45.0f;  // move on after arriving
	constexpr float kNavGoalFailAvoid = 60.0f;     // skip failed targets
	constexpr float kNavGoalNoneRetry = 5.0f;      // nothing to pick
	constexpr int kNavGoalMaxRetries = 2;          // re-issue after interruptions
	constexpr int kNavGoalMaxAttempts = 6;         // including retries after a learned block

	struct EmpiresNavGoalCandidate
	{
		int entity;
		Vector origin;
		float distance;
		const char* className;
	};

	// Enemy on the point or the game reports its capture running (status 3,
	// seen with rising progress in recorder sessions).
	bool empiresPointUnderAttack(const CEmpiresMod::CapturePointObservation& point, const int team)
	{
		if (team == EMP_TEAM_NF)
			return !point.beOccupantUserIds.empty() || (point.beStatusKnown && point.beStatus == 3);
		return !point.nfOccupantUserIds.empty() || (point.nfStatusKnown && point.nfStatus == 3);
	}

	// A spot 400-1200 around the flag that is reachable from it, upper levels
	// included (as_escort NF spawn ground is higher than flag 5). preferHigh asks
	// for at least 64 above the flag first.
	bool pickEmpiresDefendSpot(const Vector& flag, const bool preferHigh, Vector& spot)
	{
		constexpr float kMin = 400.0f;
		constexpr float kMax = 1200.0f;
		const auto ground = RCBotNavMesh::CollectLocalGroundSamples(flag, kMax + 400.0f, 0, 0, 100000);
		std::vector<Vector> all;
		std::vector<Vector> high;
		for (const auto& sample : ground.samples)
		{
			const float d = (sample.origin - flag).Length2D();
			if (d < kMin || d > kMax)
				continue;
			all.push_back(sample.origin);
			if (sample.origin.z > flag.z + 64.0f)
				high.push_back(sample.origin);
		}
		for (int pass = 0; pass < 2; ++pass)
		{
			const std::vector<Vector>& pool = pass == 0 && preferHigh ? high : all;
			for (int attempt = 0; attempt < 6 && !pool.empty(); ++attempt)
			{
				const Vector& candidate = pool[static_cast<std::size_t>(std::rand()) % pool.size()];
				RCBotNavMesh::Path path;
				if (RCBotNavMesh::BuildPath(flag, candidate, path) == RCBotNavMesh::PathResult::SUCCESS)
				{
					spot = candidate;
					return true;
				}
			}
		}
		return false;
	}

	float empiresNavGoalDistance2D(const Vector& a, const Vector& b)
	{
		return (Vector(a.x, a.y, 0.0f) - Vector(b.x, b.y, 0.0f)).Length();
	}
}

void CBotEmpires::endEmpiresNavmeshGoal(const char* event, const char* reason,
	const float avoidSeconds)
{
	const float now = gpGlobals != nullptr ? gpGlobals->curtime : 0.0f;
	Msg("[RCBOT2] goal-roam %s userid=%d type=%s entity=%d class=%s reason=%s dist=%.0f elapsed=%.1f avoid=%.0f bot=(%.0f %.0f %.0f)\n",
		event, engine != nullptr && m_pEdict != nullptr ? engine->GetPlayerUserId(m_pEdict) : -1,
		empiresNavGoalTypeName(m_iEmpiresNavGoalType), m_iEmpiresNavGoalEntity,
		m_szEmpiresNavGoalClass, reason,
		m_pEdict != nullptr ? empiresNavGoalDistance2D(getOrigin(), m_vEmpiresNavGoalOrigin) : -1.0f,
		now - m_flEmpiresNavGoalStart, avoidSeconds,
		m_pEdict != nullptr ? getOrigin().x : 0.0f, m_pEdict != nullptr ? getOrigin().y : 0.0f,
		m_pEdict != nullptr ? getOrigin().z : 0.0f);
	if (avoidSeconds > 0.0f && m_iEmpiresNavGoalEntity > 0)
	{
		auto slot = std::find_if(m_EmpiresNavGoalAvoid.begin(), m_EmpiresNavGoalAvoid.end(),
			[&](const EmpiresNavGoalAvoid& entry) { return entry.entity == m_iEmpiresNavGoalEntity; });
		if (slot == m_EmpiresNavGoalAvoid.end())
			slot = std::find_if(m_EmpiresNavGoalAvoid.begin(), m_EmpiresNavGoalAvoid.end(),
				[&](const EmpiresNavGoalAvoid& entry) { return entry.until <= now; });
		if (slot == m_EmpiresNavGoalAvoid.end())
		{
			m_EmpiresNavGoalAvoid.push_back(EmpiresNavGoalAvoid{});
			slot = m_EmpiresNavGoalAvoid.end() - 1;
		}
		slot->entity = m_iEmpiresNavGoalEntity;
		slot->until = now + avoidSeconds;
	}
	m_bEmpiresNavGoalActive = false;
	m_iEmpiresNavGoalRetries = 0;
	m_iEmpiresNavGoalAttempts = 0;
}


// ---------------------------------------------------------------------------
// Hostile MG turrets. as_escort: the map's NF turret at (4526 3000) killed BE
// infantry ~1500 away five times in four minutes; the kill shows up as
// suicide with "world" and player_hurt carries no attacker. Ranges from the
// building scripts (scripts_000.vpk "turret range"): MG level 1/2/3 = 2000/3000/4000.
extern IEngineTrace* enginetrace;

namespace
{
	constexpr float kTurretThreatSeconds = 60.0f;   // keep out of its circle this long
	constexpr float kTurretRetreatMargin = 250.0f;  // retreat this far beyond the range
	constexpr float kTurretRetreatTimeout = 20.0f;

	// One known turret per team, shared by all its bots: whoever is hit first
	// warns the rest (as_escort: the second BE bot walked in after the first knew).
	struct TeamTurretThreat
	{
		int entity = 0;
		Vector origin;
		float range = 0.0f;
		float until = 0.0f;
	};
	TeamTurretThreat s_teamTurretThreat[4];

	// Shortest 2D distance from p to the segment a-b.
	float empiresSegmentDistance2D(const Vector& a, const Vector& b, const Vector& p)
	{
		const float dx = b.x - a.x, dy = b.y - a.y;
		const float lengthSq = dx * dx + dy * dy;
		float t = lengthSq > 1.0f ? ((p.x - a.x) * dx + (p.y - a.y) * dy) / lengthSq : 0.0f;
		t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
		const float ex = a.x + dx * t - p.x, ey = a.y + dy * t - p.y;
		return std::sqrt(ex * ex + ey * ey);
	}

	float empiresMgTurretRange(edict_t* pEdict)
	{
		IServerEntity* entity = pEdict != nullptr ? pEdict->GetIServerEntity() : nullptr;
		const char* model = entity != nullptr ? STRING(entity->GetModelName()) : nullptr;
		if (model != nullptr && std::strstr(model, "lvl3") != nullptr)
			return 4000.0f;
		if (model != nullptr && std::strstr(model, "lvl2") != nullptr)
			return 3000.0f;
		return 2000.0f;
	}
}

namespace
{
	class CTraceFilterTurretRocket : public CTraceFilter
	{
	public:
		explicit CTraceFilterTurretRocket(const IHandleEntity* shooter) : m_shooter(shooter) {}
		bool ShouldHitEntity(IHandleEntity* entity, int) override
		{
			const ICollideable* collision = enginetrace->GetCollideable(entity);
			return entity != m_shooter &&
				(collision == nullptr || collision->GetCollisionGroup() != COLLISION_GROUP_VEHICLE_CLIP);
		}

	private:
		const IHandleEntity* m_shooter;
	};

	bool empiresTurretRocketAim(const Vector& eye, edict_t* shooter, edict_t* turret, QAngle& aim)
	{
		if (enginetrace == nullptr || turret->GetUnknown() == nullptr)
			return false;
		const Vector origin = CBotGlobals::entityOrigin(turret);
		CBaseEntity* target = turret->GetUnknown()->GetBaseEntity();
		CTraceFilterTurretRocket filter(shooter->GetIServerEntity());
		for (const float height : {40.0f, 24.0f, 56.0f})
		{
			const Vector point = origin + Vector(0, 0, height);
			VectorAngles(point - eye, aim);
			Vector forward, right;
			AngleVectors(aim, &forward, &right, nullptr);
			// CWeaponRPG::PrimaryAttack launches 12 forward and 6 right of the eye.
			const Vector muzzle = eye + forward * 12.0f + right * 6.0f;
			VectorAngles(point - muzzle, aim);
			Ray_t ray;
			// CEmpBaseMissile::Spawn uses a four-unit cube. The game's
			// projectile mask also includes grates and opaque solids.
			ray.Init(muzzle, point, Vector(-2, -2, -2), Vector(2, 2, 2));
			trace_t hit;
			constexpr unsigned int kMissileMask = 0x42c0608b;
			enginetrace->TraceRay(ray, kMissileMask, &filter, &hit);
			// Both RPGs have a 50-unit blast radius. Leave some margin for
			// the collision hull and impact point.
			if (!hit.startsolid && hit.DidHit() &&
				(hit.m_pEnt == target || hit.endpos.DistToSqr(point) <= 40.0f * 40.0f))
				return true;
		}
		return false;
	}
}

bool CBotEmpires::handleEmpiresTurretRocketFallback(edict_t* turret, const float range,
	const int health, bool logFailure)
{
	const char* weaponName = getTeam() == EMP_TEAM_BE ? "weapon_emp_imp_rpg" : "weapon_emp_nf_rpg";
	const CBaseHandle* weapons = CClassInterface::getWeaponList(m_pEdict);
	if (weapons == nullptr)
		return false;
	edict_t* launcher = nullptr;
	for (int slot = 0; slot < MAX_WEAPONS; ++slot)
	{
		const int index = weapons[slot].GetEntryIndex();
		edict_t* weapon = index > 0 ? INDEXENT(index) : nullptr;
		if (weapon != nullptr && !weapon->IsFree() && weapon->GetClassName() != nullptr &&
			std::strcmp(weapon->GetClassName(), weaponName) == 0 &&
			CClassInterface::getWeaponState(weapon) != WEAPON_NOT_CARRIED)
		{
			launcher = weapon;
			break;
		}
	}
	int ammoType = -1, secondary = -1;
	if (launcher != nullptr)
		CClassInterface::getAmmoTypes(launcher, &ammoType, &secondary);
	if (ammoType < 0 || getAmmo(ammoType) <= 0)
	{
		if (logFailure)
			Msg("[RCBOT2] turret-rpg unavailable userid=%d reason=%s\n",
				engine->GetPlayerUserId(m_pEdict), launcher == nullptr ? "no-launcher" : "no-ammo");
		return false;
	}

	const Vector origin = getOrigin();
	const Vector target = CBotGlobals::entityOrigin(turret);
	const float distance = empiresNavGoalDistance2D(origin, target);
	const int userId = engine->GetPlayerUserId(m_pEdict);
	const int turretIndex = engine->IndexOfEdict(turret);
	constexpr float kRocketMargin = 1100.0f;
	if (distance >= range + 5.0f && distance <= range + kRocketMargin &&
		empiresTurretRocketAim(getEyePosition(), m_pEdict, turret, m_qEmpiresTurretRocketAim))
	{
		cancelNavigationAttempt("turret-rpg");
		m_pSchedules->freeMemory();
		if (m_bEmpiresNavGoalActive)
			endEmpiresNavmeshGoal("drop", "turret-rpg", 0.0f);
		stopMoving();
		m_pButtons->letGo(IN_ATTACK);
		m_pButtons->letGo(IN_ATTACK2);
		m_pButtons->letGo(IN_RELOAD);
		m_iButtons = 0;
		m_hEmpiresTurretRocketTarget = turret;
		m_hEmpiresTurretRocketWeapon = launcher;
		m_iEmpiresTurretRocketAmmoType = ammoType;
		m_iEmpiresTurretRocketAmmo = getAmmo(ammoType);
		m_iEmpiresTurretRocketHealth = health;
		m_bEmpiresTurretRocketFired = false;
		m_bEmpiresTurretRocketApproach = false;
		m_flEmpiresTurretRocketUntil = gpGlobals->curtime + 12.0f;
		selectWeaponName(weaponName);
		Msg("[RCBOT2] turret-rpg armed userid=%d turret=%d dist=%.0f health=%d\n",
			userId, turretIndex, distance, health);
		return true;
	}

	const auto ground = RCBotNavMesh::CollectLocalGroundSamples(target, range + kRocketMargin + 200.0f, 0, 0, 100000);
	std::vector<Vector> visibleSpots;
	for (const auto& sample : ground.samples)
	{
		const Vector& spot = sample.origin;
		const float distance = empiresNavGoalDistance2D(spot, target);
		QAngle aim;
		if (distance >= range + 40.0f && distance <= range + kRocketMargin &&
			spot.DistToSqr(origin) > 64.0f * 64.0f &&
			spot.DistToSqr(m_vEmpiresTurretLastFireSpot) > 64.0f * 64.0f &&
			empiresTurretRocketAim(spot + Vector(0, 0, 64), m_pEdict, turret, aim))
			visibleSpots.push_back(spot);
	}
	std::sort(visibleSpots.begin(), visibleSpots.end(), [&](const Vector& a, const Vector& b) {
		return a.DistToSqr(origin) < b.DistToSqr(origin);
	});
	for (const Vector& spot : visibleSpots)
	{
		RCBotNavMesh::Path path;
		if (RCBotNavMesh::BuildPath(origin, spot, path, this) != RCBotNavMesh::PathResult::SUCCESS)
			continue;
		if (m_bEmpiresNavGoalActive)
			endEmpiresNavmeshGoal("drop", "turret-rpg", 0.0f);
		if (!requestNavigation(spot, RCBotNavigation::NavigationPolicy::NAVMESH_ONLY, 0))
			break;
		m_vEmpiresTurretLastFireSpot = spot;
		m_bEmpiresTurretRocketApproach = true;
		m_flEmpiresTurretFireMoveUntil = gpGlobals->curtime + 30.0f;
		Msg("[RCBOT2] turret-rpg move userid=%d turret=%d to=(%.0f %.0f %.0f) dist=%.0f\n",
			userId, turretIndex, spot.x, spot.y, spot.z, empiresNavGoalDistance2D(spot, target));
		return true;
	}
	if (logFailure)
		Msg("[RCBOT2] turret-rpg unavailable userid=%d turret=%d reason=no-reachable-spot visible=%zu\n",
			userId, turretIndex, visibleSpots.size());
	return false;
}

void CBotEmpires::tickEmpiresTurretRocket()
{
	edict_t* turret = m_hEmpiresTurretRocketTarget.get();
	edict_t* weapon = m_hEmpiresTurretRocketWeapon.get();
	const float now = gpGlobals->curtime;
	int health = 0;
	const bool targetAlive = readEmpiresEntityIntProperty(turret, "m_iHealth", health) && health > 0;
	const bool hit = m_bEmpiresTurretRocketFired && health < m_iEmpiresTurretRocketHealth;
	if (hit || !targetAlive || weapon == nullptr || m_hEmpiresTurretHitBy.get() != turret ||
		now >= m_flEmpiresTurretRocketUntil)
	{
		Msg("[RCBOT2] turret-rpg result=%s userid=%d health_before=%d health=%d fired=%d\n",
			hit ? "hit" : "fail", engine->GetPlayerUserId(m_pEdict), m_iEmpiresTurretRocketHealth,
			health, m_bEmpiresTurretRocketFired);
		m_flEmpiresTurretRocketUntil = 0.0f;
		m_flEmpiresTurretMortarNext = now + (hit ? 0.5f : 10.0f);
		m_pButtons->letGo(IN_ATTACK);
		m_pButtons->letGo(IN_RELOAD);
		m_iButtons &= ~(IN_ATTACK | IN_RELOAD);
		return;
	}
	edict_t* active = CClassInterface::getCurrentWeapon(m_pEdict);
	const char* expected = getTeam() == EMP_TEAM_BE ? "weapon_emp_imp_rpg" : "weapon_emp_nf_rpg";
	if (active == nullptr || active->IsFree() || active->GetClassName() == nullptr ||
		std::strcmp(active->GetClassName(), expected) != 0)
	{
		selectWeaponName(expected);
		return;
	}
	weapon = active;
	m_hEmpiresTurretRocketWeapon = weapon;

	if (!m_bEmpiresTurretRocketFired &&
		getAmmo(m_iEmpiresTurretRocketAmmoType) < m_iEmpiresTurretRocketAmmo)
	{
		m_bEmpiresTurretRocketFired = true;
		m_pButtons->letGo(IN_ATTACK);
		Msg("[RCBOT2] turret-rpg fired userid=%d turret=%d ammo=%d\n",
			engine->GetPlayerUserId(m_pEdict), engine->IndexOfEdict(turret),
			getAmmo(m_iEmpiresTurretRocketAmmoType));
	}
	if (m_bEmpiresTurretRocketFired)
		return;

	bool loaded = false;
	if (!readEmpiresBoolProperty(m_pEdict, "m_bRPGLoaded", loaded))
		return;
	if (!loaded)
	{
		m_pButtons->tap(IN_RELOAD);
		return;
	}
	float nextPrimary = 0.0f, nextAttack = 0.0f;
	if (readEmpiresWeaponFloatProperty(weapon, "m_flNextPrimaryAttack", nextPrimary) &&
		readEmpiresDebugFloatProperty(m_pEdict, "m_flNextAttack", nextAttack) &&
		now >= nextPrimary && now >= nextAttack &&
		empiresNavGoalDistance2D(getOrigin(), CBotGlobals::entityOrigin(turret)) >=
			empiresMgTurretRange(turret) + 5.0f &&
		empiresTurretRocketAim(getEyePosition(), m_pEdict, turret, m_qEmpiresTurretRocketAim))
	{
		m_pButtons->letGo(IN_RELOAD);
		m_pButtons->tap(IN_ATTACK);
	}
}

void CBotEmpires::noteEmpiresTurretThreat(edict_t* pAttacker, const Vector* damageOrigin)
{
	if (gpGlobals == nullptr || engine == nullptr || m_pEdict == nullptr)
		return;
	// A player attacker within turret range shot us himself.
	if (pAttacker != nullptr && pAttacker != m_pEdict && CBotGlobals::isPlayer(pAttacker) &&
		(CBotGlobals::entityOrigin(pAttacker) - getOrigin()).Length() < 1500.0f)
		return;
	const int team = getTeam();
	const Vector eye = getEyePosition();
	float best = 0.0f;
	edict_t* turret = nullptr;
	for (int index = gpGlobals->maxClients + 1; index < gpGlobals->maxEntities; ++index)
	{
		edict_t* pEdict = INDEXENT(index);
		if (pEdict == nullptr || pEdict->IsFree())
			continue;
		const char* className = pEdict->GetClassName();
		if (className == nullptr || std::strncmp(className, "emp_building_", 13) != 0 ||
			std::strstr(className, "mgturret") == nullptr)
			continue;
		int turretTeam = -1, health = 0;
		// Map-placed turrets report no team (registry: team=-1); the class name tells it.
		if (!readEmpiresEntityIntProperty(pEdict, "m_iTeamNum", turretTeam) ||
			(turretTeam != EMP_TEAM_NF && turretTeam != EMP_TEAM_BE))
			turretTeam = std::strncmp(className, "emp_building_nf_", 16) == 0 ? EMP_TEAM_NF :
				(std::strncmp(className, "emp_building_imp_", 17) == 0 ? EMP_TEAM_BE : -1);
		if (turretTeam == team || turretTeam < 0 ||
			!readEmpiresEntityIntProperty(pEdict, "m_iHealth", health) || health <= 0)
			continue;
		const Vector origin = CBotGlobals::entityOrigin(pEdict);
		// CBasePlayer stores the inflictor's origin for the actual damage.
		// Keep this per bot and per turret, separately from inferred threats.
		if (pEdict == pAttacker ||
			(damageOrigin != nullptr && damageOrigin->DistToSqr(origin) < 1.0f))
		{
			if (m_hEmpiresTurretHitBy.get() != pEdict)
			{
				Msg("[RCBOT2] turret-hit confirmed userid=%d turret=%d\n",
					engine->GetPlayerUserId(m_pEdict), index);
				m_hEmpiresTurretHitBy = pEdict;
			}
			turret = pEdict;
			best = (origin - eye).Length();
			break;
		}
		if (damageOrigin != nullptr && hasSomeConditions(CONDITION_SEE_CUR_ENEMY))
			continue;
		const float range = empiresMgTurretRange(pEdict);
		const float distance = (origin - eye).Length();
		if (distance > range + 100.0f || (turret != nullptr && distance >= best))
			continue;
		if (enginetrace != nullptr)
		{
			Ray_t ray;
			ray.Init(origin + Vector(0, 0, 48), eye);
			CTraceFilterWorldOnly worldOnly;
			trace_t hit;
			enginetrace->TraceRay(ray, MASK_SHOT, &worldOnly, &hit);
			if (hit.fraction < 0.97f)
				continue;
		}
		turret = pEdict;
		best = distance;
	}
	if (turret == nullptr)
	{
		if (pAttacker == nullptr)
			Msg("[RCBOT2] turret-threat none userid=%d bot=(%.0f %.0f %.0f)\n", engine->GetPlayerUserId(m_pEdict),
				getOrigin().x, getOrigin().y, getOrigin().z);
		return;
	}
	const float now = gpGlobals->curtime;
	const int index = engine->IndexOfEdict(turret);
	const bool known = index == m_iEmpiresTurretThreat && m_flEmpiresTurretThreatUntil > now;
	m_iEmpiresTurretThreat = index;
	m_vEmpiresTurretThreatOrigin = CBotGlobals::entityOrigin(turret);
	m_flEmpiresTurretThreatRange = empiresMgTurretRange(turret);
	m_flEmpiresTurretThreatUntil = now + kTurretThreatSeconds;
	const int myTeam = getTeam();
	if (myTeam >= 0 && myTeam < 4)
	{
		s_teamTurretThreat[myTeam] = { index, m_vEmpiresTurretThreatOrigin, m_flEmpiresTurretThreatRange,
			m_flEmpiresTurretThreatUntil };
		RCBotNavMesh::SetTeamDangerCircle(myTeam, m_vEmpiresTurretThreatOrigin, m_flEmpiresTurretThreatRange);
	}
	if (!m_bEmpiresTurretRetreating)
		m_bEmpiresTurretRetreatPending = true;
	if (!known)
		Msg("[RCBOT2] turret-threat userid=%d turret=%d class=%s pos=(%.0f %.0f %.0f) range=%.0f dist=%.0f bot=(%.0f %.0f %.0f)\n",
			engine->GetPlayerUserId(m_pEdict), index, turret->GetClassName(),
			m_vEmpiresTurretThreatOrigin.x, m_vEmpiresTurretThreatOrigin.y, m_vEmpiresTurretThreatOrigin.z,
			m_flEmpiresTurretThreatRange, best, getOrigin().x, getOrigin().y, getOrigin().z);
}

bool CBotEmpires::isInsideEmpiresTurretThreat(const Vector& point)
{
	return isEmpiresTurretThreatOnLine(point, point);
}

// The team's known turret covers some part of the straight line a-b.
bool CBotEmpires::isEmpiresTurretThreatOnLine(const Vector& a, const Vector& b)
{
	const int team = getTeam();
	if (gpGlobals == nullptr || team < 0 || team >= 4)
		return false;
	TeamTurretThreat& threat = s_teamTurretThreat[team];
	// Riflemen cannot fight a turret: keep out of its circle as long as it stands,
	// not only for kTurretThreatSeconds (as_escort: bots walked back in after 60 s).
	if (threat.entity <= 0)
		return false;
	edict_t* turret = INDEXENT(threat.entity);
	const char* className = turret != nullptr && !turret->IsFree() ? turret->GetClassName() : nullptr;
	int health = 0;
	if (className == nullptr || std::strstr(className, "mgturret") == nullptr ||
		!readEmpiresEntityIntProperty(turret, "m_iHealth", health) || health <= 0)
	{
		Msg("[RCBOT2] turret-destroyed team=%d turret=%d\n", team, threat.entity);
		threat.entity = 0;
		RCBotNavMesh::SetTeamDangerCircle(team, threat.origin, 0.0f);
		return false;
	}
	return empiresSegmentDistance2D(a, b, threat.origin) < threat.range;
}

bool CBotEmpires::handleEmpiresTurretThreat()
{
	if (gpGlobals == nullptr || engine == nullptr || m_pSchedules == nullptr)
		return false;
	const float now = gpGlobals->curtime;
	const int userId = engine->GetPlayerUserId(m_pEdict);
	if (m_bEmpiresTurretRetreating)
	{
		const float dist = empiresNavGoalDistance2D(getOrigin(), m_vEmpiresTurretRetreatPoint);
		const bool outside = !isInsideEmpiresTurretThreat(getOrigin());
		if (dist < 120.0f || (outside && m_pSchedules->isEmpty()) || now > m_flEmpiresTurretRetreatDeadline)
		{
			m_bEmpiresTurretRetreating = false;
			Msg("[RCBOT2] turret-retreat end userid=%d reason=%s dist=%.0f turret_dist=%.0f\n", userId,
				dist < 120.0f ? "arrived" : (now > m_flEmpiresTurretRetreatDeadline ? "timeout" : "outside"),
				dist, empiresNavGoalDistance2D(getOrigin(), m_vEmpiresTurretThreatOrigin));
			return false;
		}
		// The route drained early: ask again.
		if (m_pSchedules->isEmpty())
			requestNavigation(m_vEmpiresTurretRetreatPoint, RCBotNavigation::NavigationPolicy::NAVMESH_ONLY, 0);
		return true;
	}
	if (!m_bEmpiresTurretRetreatPending)
		return false;
	m_bEmpiresTurretRetreatPending = false;
	if (!isInsideEmpiresTurretThreat(getOrigin()))
		return false;

	// Straight away from the turret, or up to 60 degrees off it; nearest mesh point
	// that lies outside the range, closest to the bot.
	Vector away = getOrigin() - m_vEmpiresTurretThreatOrigin;
	away.z = 0.0f;
	if (away.Length2D() < 1.0f)
		away = Vector(1, 0, 0);
	away.NormalizeInPlace();
	const float baseYaw = std::atan2(away.y, away.x);
	bool found = false;
	Vector best;
	float bestDistance = 0.0f;
	for (const float offset : { 0.0f, 30.0f, -30.0f, 60.0f, -60.0f })
	{
		const float yaw = baseYaw + offset * static_cast<float>(M_PI) / 180.0f;
		const Vector wanted = m_vEmpiresTurretThreatOrigin +
			Vector(std::cos(yaw), std::sin(yaw), 0.0f) * (m_flEmpiresTurretThreatRange + kTurretRetreatMargin);
		Vector onMesh;
		if (!RCBotNavMesh::ClosestPointOnMesh(Vector(wanted.x, wanted.y, getOrigin().z), 600.0f, onMesh) ||
			empiresNavGoalDistance2D(onMesh, m_vEmpiresTurretThreatOrigin) < m_flEmpiresTurretThreatRange + 50.0f ||
			std::fabs(onMesh.z - getOrigin().z) > 400.0f)
			continue;
		const float distance = (onMesh - getOrigin()).Length();
		if (!found || distance < bestDistance)
		{
			found = true;
			best = onMesh;
			bestDistance = distance;
		}
	}
	if (!found)
	{
		Msg("[RCBOT2] turret-retreat none userid=%d turret=%d\n", userId, m_iEmpiresTurretThreat);
		return false;
	}
	if (m_bEmpiresNavGoalActive)
		endEmpiresNavmeshGoal("drop", "turret-threat", 0.0f);
	cancelNavigationAttempt("turret-retreat");
	m_pSchedules->freeMemory();
	if (!requestNavigation(best, RCBotNavigation::NavigationPolicy::NAVMESH_ONLY, 0))
	{
		Msg("[RCBOT2] turret-retreat rejected userid=%d\n", userId);
		return false;
	}
	m_bEmpiresTurretRetreating = true;
	m_vEmpiresTurretRetreatPoint = best;
	m_flEmpiresTurretRetreatDeadline = now + kTurretRetreatTimeout;
	Msg("[RCBOT2] turret-retreat start userid=%d turret=%d to=(%.0f %.0f %.0f) dist=%.0f\n",
		userId, m_iEmpiresTurretThreat, best.x, best.y, best.z, bestDistance);
	return true;
}

// Grenadier against the team's known turret: shell it with the mortar from just
// outside its range (the shell arcs, no sight line needed). Mortar shell 1800 u/s,
// so on flat ground it reaches about 4000.
bool CBotEmpires::handleEmpiresGrenadierTurretAttack()
{
	if (gpGlobals == nullptr || engine == nullptr || m_pSchedules == nullptr)
		return false;
	const int team = getTeam();
	if (team < 0 || team >= 4 || s_teamTurretThreat[team].entity <= 0)
		return false;
	// Validates the turret and forgets it once destroyed.
	if (!isEmpiresTurretThreatOnLine(s_teamTurretThreat[team].origin, s_teamTurretThreat[team].origin))
		return false;
	int classId = -1;
	bool classDesired = false;
	bool classDesiredKnown = false;
	const char* classProperty = nullptr;
	if (!readEmpiresClass(m_pEdict, classId, classDesired, classDesiredKnown, classProperty) ||
		classId != EMP_CLASS_GRENADIER)
		return false;

	const TeamTurretThreat& threat = s_teamTurretThreat[team];
	edict_t* turret = INDEXENT(threat.entity);
	if (m_hEmpiresTurretFireTarget.get() != turret)
	{
		m_hEmpiresTurretFireTarget = turret;
		m_iEmpiresTurretMortarFails = 0;
		m_iEmpiresTurretMortarShots = 0;
		m_bEmpiresTurretMortarWasActive = false;
		m_bEmpiresTurretOutOfReachLogged = false;
		m_bEmpiresTurretRocketApproach = false;
		m_vEmpiresTurretLastFireSpot = vec3_origin;
		m_vEmpiresTurretFailedMortarSpot = vec3_origin;
		m_flEmpiresTurretMortarNext = 0.0f;
		m_flEmpiresTurretFireMoveNext = 0.0f;
		m_flEmpiresTurretFireMoveUntil = 0.0f;
	}
	const bool canUseRocket = threat.range == 4000.0f && m_hEmpiresTurretHitBy.get() == turret;
	const float now = gpGlobals->curtime;
	const int userId = engine->GetPlayerUserId(m_pEdict);
	int health = 0;
	readEmpiresEntityIntProperty(turret, "m_iHealth", health);

	// A shot is under way: let the mortar action finish.
	if (isEmpiresMortarActionActive())
	{
		m_bEmpiresTurretMortarWasActive = true;
		return true;
	}
	if (m_bEmpiresTurretMortarWasActive)
	{
		m_bEmpiresTurretMortarWasActive = false;
		const bool ok = m_eEmpiresMortarActionState == EmpiresMortarActionState::COMPLETE;
		m_iEmpiresTurretMortarFails = ok ? 0 : m_iEmpiresTurretMortarFails + 1;
		if (ok)
			++m_iEmpiresTurretMortarShots;
		else
			m_vEmpiresTurretFailedMortarSpot = m_vEmpiresTurretSettleOrigin;
		m_flEmpiresTurretMortarNext = now + (ok ? 0.5f : 0.0f);
		m_flEmpiresTurretFireMoveUntil = 0.0f;
		Msg("[RCBOT2] turret-mortar shot userid=%d turret=%d result=%s health=%d fails=%d shots=%d\n",
			userId, threat.entity, ok ? "fired" : "fail", health, m_iEmpiresTurretMortarFails,
			m_iEmpiresTurretMortarShots);
		if (canUseRocket && m_iEmpiresTurretMortarFails >= 2 &&
			handleEmpiresTurretRocketFallback(turret, threat.range, health, false))
			return true;
		if (m_iEmpiresTurretMortarFails >= 3)
		{
			m_iEmpiresTurretMortarFails = 0;
			m_flEmpiresTurretMortarNext = now + 30.0f;
			return false;
		}
	}
	if (now < m_flEmpiresTurretMortarNext && m_flEmpiresTurretMortarNext - now > 1.0f)
		return false;
	if (m_bEmpiresTurretRocketApproach)
	{
		if (canUseRocket && !m_pSchedules->isEmpty() && now < m_flEmpiresTurretFireMoveUntil)
			return true;
		m_bEmpiresTurretRocketApproach = false;
		if (canUseRocket && handleEmpiresTurretRocketFallback(turret, threat.range, health, false))
			return true;
	}

	// Farthest 2D distance a shell from height 'fromZ' reaches the turret base:
	// d^2 = v^4/g^2 - 2 h v^2/g (low and high arc meet at 45 degrees).
	const float target_z = threat.origin.z + 16.0f;
	auto maxReach = [&](const float fromZ) {
		const double v2 = 1800.0 * 1800.0, g = 800.0;
		const double h = target_z - (fromZ + 48.0);
		const double d2 = v2 * v2 / (g * g) - 2.0 * h * v2 / g;
		return d2 > 0.0 ? static_cast<float>(std::sqrt(d2)) : 0.0f;
	};

	const Vector target(threat.origin.x, threat.origin.y, target_z);
	auto clearArc = [&](const Vector& eye, Vector& hitPoint) {
		QAngle angles;
		Vector launchOrigin;
		return findEmpiresMortarAim(eye, target, true, angles, launchOrigin, hitPoint);
	};

	const Vector origin = getOrigin();
	const float distance = empiresNavGoalDistance2D(origin, threat.origin);
	// The walk ends up to ~64 short of the spot, so fire anywhere past the range itself.
	const float inner = threat.range + 30.0f;
	const float outer = std::min(threat.range + 450.0f, maxReach(origin.z) - 20.0f);
	if (distance >= threat.range + 5.0f && distance <= outer &&
		(m_iEmpiresTurretMortarFails == 0 ||
		 origin.DistToSqr(m_vEmpiresTurretFailedMortarSpot) > 64.0f * 64.0f))
	{
		if (now < m_flEmpiresTurretMortarNext)
			return true;
		Vector hitPoint;
		if (clearArc(origin + Vector(0, 0, 44), hitPoint))
		{
			// Let the grenadier stop before the action calculates its launch point.
			if (m_vEmpiresTurretSettleOrigin.DistTo(origin) > 4.0f)
			{
				m_vEmpiresTurretSettleOrigin = origin;
				m_flEmpiresTurretSettledSince = now;
				cancelNavigationAttempt("turret-mortar-settle");
				m_pSchedules->freeMemory();
				if (m_bEmpiresNavGoalActive)
					endEmpiresNavmeshGoal("drop", "turret-mortar", 0.0f);
			}
			if (now - m_flEmpiresTurretSettledSince < 1.0f)
			{
				stopMoving();
				return true;
			}
			m_pSchedules->freeMemory();
			if (m_bEmpiresNavGoalActive)
				endEmpiresNavmeshGoal("drop", "turret-mortar", 0.0f);
			// CEmpShell stamps m_flSpawn with realtime but fuses on curtime - m_flSpawn > 30.
			// Under host_timescale curtime outruns realtime and every shell bursts at the muzzle.
			if (gpGlobals != nullptr && gpGlobals->curtime - gpGlobals->realtime > 25.0f)
			{
				Msg("[RCBOT2] turret-mortar unavailable userid=%d reason=timescale-shell-fuse cur-real=%.1f\n",
					userId, gpGlobals->curtime - gpGlobals->realtime);
				m_flEmpiresTurretMortarNext = now + 30.0f;
				return false;
			}
			if (!requestEmpiresMortarAtPointOnce(target, true))
			{
				Msg("[RCBOT2] turret-mortar unavailable userid=%d\n", userId);
				m_flEmpiresTurretMortarNext = now + 30.0f;
				return false;
			}
			m_bEmpiresTurretMortarWasActive = true;
			Msg("[RCBOT2] turret-mortar fire userid=%d turret=%d dist=%.0f range=%.0f health=%d bot=(%.0f %.0f %.0f)\n",
				userId, threat.entity, distance, threat.range, health, origin.x, origin.y, origin.z);
			return true;
		}
	}

	// Search the ring just outside range, starting from the bot's side.
	if (!m_pSchedules->isEmpty() && now < m_flEmpiresTurretFireMoveUntil)
		return true;
	if (now < m_flEmpiresTurretFireMoveNext)
		return true;
	m_flEmpiresTurretFireMoveNext = now + 3.0f;
	const auto ground = RCBotNavMesh::CollectLocalGroundSamples(threat.origin,
		threat.range + 650.0f, 0, 0, 100000);
	std::vector<Vector> candidates;
	for (const auto& sample : ground.samples)
	{
		const Vector& spot = sample.origin;
		const float distance = empiresNavGoalDistance2D(spot, threat.origin);
		if (distance >= inner &&
			distance <= std::min(threat.range + 450.0f, maxReach(spot.z) - 20.0f) &&
			spot.DistToSqr(origin) > 64.0f * 64.0f &&
			spot.DistToSqr(m_vEmpiresTurretLastFireSpot) > 64.0f * 64.0f &&
			(m_iEmpiresTurretMortarFails == 0 ||
			 spot.DistToSqr(m_vEmpiresTurretFailedMortarSpot) > 64.0f * 64.0f))
			candidates.push_back(spot);
	}
	std::sort(candidates.begin(), candidates.end(), [&](const Vector& a, const Vector& b) {
		return a.DistToSqr(origin) < b.DistToSqr(origin);
	});
	int arcFails = 0, routeFails = 0;
	Vector firstHit(0, 0, 0);
	for (const Vector& spot : candidates)
	{
		Vector hitPoint;
		if (!clearArc(spot + Vector(0, 0, 44), hitPoint))
		{
			if (arcFails++ == 0)
				firstHit = hitPoint;
			continue;
		}
		RCBotNavMesh::Path path;
		if (RCBotNavMesh::BuildPath(origin, spot, path, this) != RCBotNavMesh::PathResult::SUCCESS)
		{
			++routeFails;
			continue;
		}
		if (m_bEmpiresNavGoalActive)
			endEmpiresNavmeshGoal("drop", "turret-mortar", 0.0f);
		if (!requestNavigation(spot, RCBotNavigation::NavigationPolicy::NAVMESH_ONLY, 0))
			break;
		m_vEmpiresTurretLastFireSpot = spot;
		m_flEmpiresTurretFireMoveUntil = now + 30.0f;
		m_bEmpiresTurretOutOfReachLogged = false;
		Msg("[RCBOT2] turret-mortar move userid=%d turret=%d to=(%.0f %.0f %.0f) spot_dist=%.0f\n",
			userId, threat.entity, spot.x, spot.y, spot.z, empiresNavGoalDistance2D(spot, threat.origin));
		return true;
	}
	const bool firstNoSpot = !m_bEmpiresTurretOutOfReachLogged;
	if (firstNoSpot)
		Msg("[RCBOT2] turret-mortar no-spot userid=%d turret=%d range=%.0f reach=%.0f dist=%.0f bot=(%.0f %.0f %.0f) candidates=%zu arc_fail=%d route_fail=%d first_hit=(%.0f %.0f %.0f)\n",
			userId, threat.entity, threat.range, maxReach(origin.z), distance, origin.x, origin.y, origin.z,
			candidates.size(), arcFails, routeFails, firstHit.x, firstHit.y, firstHit.z);
	m_bEmpiresTurretOutOfReachLogged = true;
	if (canUseRocket)
	{
		if (handleEmpiresTurretRocketFallback(turret, threat.range, health, firstNoSpot))
			return true;
	}
	m_flEmpiresTurretMortarNext = now + 10.0f;
	return false;
}

bool CBotEmpires::handleEmpiresNavmeshGoal()
{
	if (m_pSchedules == nullptr || gpGlobals == nullptr || engine == nullptr ||
		m_pEdict == nullptr || m_pEdict->IsFree())
		return false;
	const int team = getTeam();
	if (team != EMP_TEAM_NF && team != EMP_TEAM_BE)
		return false;

	const float now = gpGlobals->curtime;

	// Holding a capture point: stay put until the hold ends.
	if (m_flEmpiresNavGoalHoldUntil > now && m_pSchedules->isEmpty() && m_iEmpiresDefendHoldEntity > 0)
	{
		const CEmpiresMod* pHoldMod = static_cast<const CEmpiresMod*>(CBotGlobals::getCurrentMod());
		bool alarm = false;
		for (const CEmpiresMod::CapturePointObservation& point : pHoldMod->getCapturePoints())
			if (point.entityIndex == m_iEmpiresDefendHoldEntity)
			{
				alarm = empiresPointUnderAttack(point, team) &&
					empiresNavGoalDistance2D(getOrigin(), m_vEmpiresDefendHoldOrigin) > 200.0f;
				break;
			}
		if (alarm)
		{
			Msg("[RCBOT2] goal-roam defend-hold-break userid=%d entity=%d reason=alarm\n",
				engine->GetPlayerUserId(m_pEdict), m_iEmpiresDefendHoldEntity);
			m_flEmpiresNavGoalHoldUntil = 0.0f;
		}
		else
			handleEmpiresDefendMines(now);
	}
	if (m_flEmpiresNavGoalHoldUntil > now && m_pSchedules->isEmpty())
	{
		if (m_bEmpiresNavGoalArrivedAtCapture)
		{
			const CEmpiresMod* pMod = static_cast<const CEmpiresMod*>(CBotGlobals::getCurrentMod());
			for (const CEmpiresMod::CapturePointObservation& point : pMod->getCapturePoints())
			{
				if (point.entityIndex != m_iEmpiresNavGoalEntity)
					continue;
				if (point.teamOwnerKnown && point.teamOwner == team)
				{
					m_bEmpiresNavGoalArrivedAtCapture = false;
					const std::string detail = "point=" +
						std::to_string(point.pointIndex) + ";navmesh-goal";
					EmpiresAnalysis::RecordBotEvent(this, "CAPTURE_COMPLETED",
						nullptr, detail.c_str());
				}
				break;
			}
		}
		return true;
	}
	m_flEmpiresNavGoalHoldUntil = 0.0f;
	m_bEmpiresNavGoalArrivedAtCapture = false;
	m_iEmpiresDefendHoldEntity = -1;
	m_flEmpiresDefendMineAimUntil = 0.0f;

	// Boarded a vehicle with a running goal: end it without a failure penalty, a foot
	// route from the seat position would only fail.
	if (m_bEmpiresNavGoalActive && EmpiresVehiclePlayerVehicle(this, m_pEdict) >= 0)
	{
		endEmpiresNavmeshGoal("drop", "in-vehicle", 0.0f);
		cancelNavigationAttempt("goal-roam-drop");
		m_pSchedules->freeMemory();
		return true;
	}

	if (m_bEmpiresNavGoalActive)
	{
		// Target still valid?
		const char* dropReason = nullptr;
		edict_t* pTarget = m_iEmpiresNavGoalEntity > 0 ?
			INDEXENT(m_iEmpiresNavGoalEntity) : nullptr;
		if (pTarget == nullptr || pTarget->IsFree() ||
			pTarget->GetClassName() == nullptr ||
			std::strcmp(pTarget->GetClassName(), m_szEmpiresNavGoalClass) != 0)
		{
			dropReason = "entity-gone";
		}
		else if (m_iEmpiresNavGoalType == NAVGOAL_CAPTURE)
		{
			const CEmpiresMod* pMod = static_cast<const CEmpiresMod*>(CBotGlobals::getCurrentMod());
			for (const CEmpiresMod::CapturePointObservation& point : pMod->getCapturePoints())
			{
				if (point.entityIndex != m_iEmpiresNavGoalEntity)
					continue;
				if (point.teamOwnerKnown && point.teamOwner == team)
					dropReason = "captured-by-team";
				break;
			}
		}
		else if (m_iEmpiresNavGoalType == NAVGOAL_DEFEND)
		{
			const CEmpiresMod* pMod = static_cast<const CEmpiresMod*>(CBotGlobals::getCurrentMod());
			for (const CEmpiresMod::CapturePointObservation& point : pMod->getCapturePoints())
			{
				if (point.entityIndex != m_iEmpiresNavGoalEntity)
					continue;
				if (point.teamOwnerKnown && point.teamOwner != team)
					dropReason = "point-lost";
				else if (empiresPointUnderAttack(point, team) &&
					empiresNavGoalDistance2D(m_vEmpiresNavGoalOrigin,
						point.groundApproachKnown ? point.groundApproachOrigin : point.origin) > 200.0f)
					dropReason = "alarm";
				break;
			}
		}
		else if (m_iEmpiresNavGoalType == NAVGOAL_BUILDING)
		{
			int buildingTeam = -1;
			if (readEmpiresEntityIntProperty(pTarget, "m_iTeamNum", buildingTeam) &&
				buildingTeam == team)
				dropReason = "building-now-friendly";
		}
		else if (m_iEmpiresNavGoalType == NAVGOAL_REPAIR)
		{
			int health = 0, maxHealth = 0;
			if (!readEmpiresEntityIntProperty(pTarget, "m_iHealth", health) ||
				!readEmpiresEntityIntProperty(pTarget, "m_iMaxHealth", maxHealth) ||
				health <= 0 || health >= maxHealth)
				dropReason = "repaired";
		}

		if (dropReason != nullptr)
		{
			endEmpiresNavmeshGoal("drop", dropReason, 0.0f);
			cancelNavigationAttempt("goal-roam-drop");
			m_pSchedules->freeMemory();
			return true;
		}

		const Vector origin = getOrigin();
		const float dist = empiresNavGoalDistance2D(origin, m_vEmpiresNavGoalOrigin);
		const float radius = empiresNavGoalArriveRadius(m_iEmpiresNavGoalType);
		const bool closeInHeight = std::fabs(origin.z - m_vEmpiresNavGoalOrigin.z) < 150.0f;
		const bool drained = m_pSchedules->isEmpty();
		// Repair needs the building's floor: the kit reaches 100 (as_escort: a route
		// that ended one storey above turret 292 counted as arrived).
		const bool repair = m_iEmpiresNavGoalType == NAVGOAL_REPAIR;
		if ((dist <= radius && closeInHeight) || (drained && dist <= radius * 2.0f && (!repair || closeInHeight)))
		{
			const bool capture = m_iEmpiresNavGoalType == NAVGOAL_CAPTURE;
			const bool defend = m_iEmpiresNavGoalType == NAVGOAL_DEFEND;
			const int heldEntity = m_iEmpiresNavGoalEntity;
			endEmpiresNavmeshGoal("arrive", drained ? "route-complete" : "in-radius",
				capture || defend ? 0.0f : kNavGoalArrivedAvoid);
			if (defend)
			{
				cancelNavigationAttempt("goal-roam-arrive");
				m_pSchedules->freeMemory();
				m_flEmpiresNavGoalHoldUntil = now + kNavGoalCaptureHold;
				m_iEmpiresDefendHoldEntity = heldEntity;
				m_vEmpiresDefendHoldOrigin = m_vEmpiresNavGoalOrigin;
				const CEmpiresMod* pHeldMod = static_cast<const CEmpiresMod*>(CBotGlobals::getCurrentMod());
				for (const CEmpiresMod::CapturePointObservation& point : pHeldMod->getCapturePoints())
					if (point.entityIndex == heldEntity)
						m_vEmpiresDefendHoldOrigin = point.groundApproachKnown ? point.groundApproachOrigin : point.origin;
				Msg("[RCBOT2] goal-roam defend-hold userid=%d entity=%d seconds=%.0f\n",
					engine->GetPlayerUserId(m_pEdict), heldEntity, kNavGoalCaptureHold);
				return true;
			}
			cancelNavigationAttempt("goal-roam-arrive");
			m_pSchedules->freeMemory();
			if (repair)
			{
				// Hand over to the nearby Engineer support now, before the next
				// goal pick takes the bot away again.
				m_flEmpiresNextEngineerSupportTime = 0.0f;
				m_flEmpiresNavGoalNextPick = now + 3.0f;
			}
			if (capture)
			{
				m_flEmpiresNavGoalHoldUntil = now + kNavGoalCaptureHold;
				m_bEmpiresNavGoalArrivedAtCapture = true;
				const std::string startDetail = "entity=" +
					std::to_string(m_iEmpiresNavGoalEntity) + ";navmesh-goal";
				EmpiresAnalysis::RecordBotEvent(this, "CAPTURE_STARTED",
					nullptr, startDetail.c_str());
				Msg("[RCBOT2] goal-roam hold userid=%d entity=%d seconds=%.0f\n",
					engine->GetPlayerUserId(m_pEdict), m_iEmpiresNavGoalEntity,
					kNavGoalCaptureHold);
			}
			return true;
		}

		if (now > m_flEmpiresNavGoalDeadline)
		{
			endEmpiresNavmeshGoal("fail", "timeout", kNavGoalFailAvoid);
			cancelNavigationAttempt("goal-roam-timeout");
			m_pSchedules->freeMemory();
			return true;
		}

		// Still on the way (or another task has the bot right now).
		if (!drained)
			return false;

		// A stuck that taught the mesh a new block (door, crate, wall) makes the next
		// route different: that retry is free. as_escort BE base: gate, crate and
		// wall used up both retries although each one moved the route.
		const bool learned = RCBotNavMesh::BlockGeneration() != m_uEmpiresNavGoalBlockMark;
		if (m_iEmpiresNavGoalAttempts < kNavGoalMaxAttempts &&
			(learned || m_iEmpiresNavGoalRetries < kNavGoalMaxRetries))
		{
			if (!learned)
				++m_iEmpiresNavGoalRetries;
			++m_iEmpiresNavGoalAttempts;
			m_uEmpiresNavGoalBlockMark = RCBotNavMesh::BlockGeneration();
			// This is a bounded, deliberate retry after the previous route
			// drained; do not spend it on the same attempt's failure cooldown.
			clearNavigationAttemptCooldownForStartAnchorRetry();
			Msg("[RCBOT2] goal-roam retry userid=%d type=%s entity=%d attempt=%d learned=%d dist=%.0f\n",
				engine->GetPlayerUserId(m_pEdict),
				empiresNavGoalTypeName(m_iEmpiresNavGoalType),
				m_iEmpiresNavGoalEntity, m_iEmpiresNavGoalAttempts, learned ? 1 : 0, dist);
			if (requestNavigation(m_vEmpiresNavGoalOrigin,
				RCBotNavigation::NavigationPolicy::NAVMESH_ONLY,
				m_iEmpiresNavGoalEntity))
				return true;
			endEmpiresNavmeshGoal("fail", "request-rejected", kNavGoalFailAvoid);
			return true;
		}

		endEmpiresNavmeshGoal("fail", "route-ended", kNavGoalFailAvoid);
		return true;
	}

	if (!m_pSchedules->isEmpty() || now < m_flEmpiresNavGoalNextPick)
		return false;

	// Seated in a vehicle (passenger of another bot's vehicle, run 20261113-122756 userid=7):
	// the origin is not a world position, so planning on foot fails with start-area-not-found.
	if (EmpiresVehiclePlayerVehicle(this, m_pEdict) >= 0)
	{
		m_flEmpiresNavGoalNextPick = now + 2.0f;
		return false;
	}

	return pickEmpiresNavmeshGoal();
}

bool CBotEmpires::pickEmpiresNavmeshGoal()
{
	const float now = gpGlobals->curtime;
	const int team = getTeam();
	const int enemyTeam = team == EMP_TEAM_NF ? EMP_TEAM_BE : EMP_TEAM_NF;
	const Vector origin = getOrigin();
	const int userId = engine->GetPlayerUserId(m_pEdict);

	auto avoided = [&](const int entity)
	{
		for (const EmpiresNavGoalAvoid& slot : m_EmpiresNavGoalAvoid)
			if (slot.entity == entity && slot.until > now)
				return true;
		return false;
	};

	std::vector<EmpiresNavGoalCandidate> tiers[NAVGOAL_TYPES];

	// 1. Capture points this team may take and does not own.
	const CEmpiresMod* pMod = static_cast<const CEmpiresMod*>(CBotGlobals::getCurrentMod());
	for (const CEmpiresMod::CapturePointObservation& point : pMod->getCapturePoints())
	{
		if (!point.originKnown || point.entityIndex <= 0 || avoided(point.entityIndex))
			continue;
		const bool cappableKnown = team == EMP_TEAM_NF ? point.nfCappableKnown : point.beCappableKnown;
		const int cappable = team == EMP_TEAM_NF ? point.nfCappable : point.beCappable;
		if (cappableKnown && cappable == 0)
			continue;
		if (point.teamOwnerKnown && point.teamOwner == team)
			continue;
		const Vector goal = point.groundApproachKnown ? point.groundApproachOrigin : point.origin;
		tiers[NAVGOAL_CAPTURE].push_back({ point.entityIndex, goal,
			empiresNavGoalDistance2D(origin, goal), "emp_cap_point" });
	}

	// 0. Defend: own points the enemy may take right now (linear maps open only the
	// front point; as_escort NF holds 1-5 and must keep BE off 5). The share of
	// defenders follows the share of points the team owns, by bot rank, so an
	// attacker team (owns little) keeps attacking.
	{
		int owned = 0;
		int total = 0;
		std::vector<EmpiresNavGoalCandidate> threatened;
		std::vector<EmpiresNavGoalCandidate> attacked;
		for (const CEmpiresMod::CapturePointObservation& point : pMod->getCapturePoints())
		{
			if (!point.originKnown || point.entityIndex <= 0 || !point.teamOwnerKnown)
				continue;
			++total;
			if (point.teamOwner != team)
				continue;
			++owned;
			const bool enemyCappableKnown = team == EMP_TEAM_NF ? point.beCappableKnown : point.nfCappableKnown;
			const int enemyCappable = team == EMP_TEAM_NF ? point.beCappable : point.nfCappable;
			if (!enemyCappableKnown || enemyCappable == 0)
				continue;
			const Vector goal = point.groundApproachKnown ? point.groundApproachOrigin : point.origin;
			threatened.push_back({ point.entityIndex, goal,
				empiresNavGoalDistance2D(origin, goal), "emp_cap_point" });
			if (empiresPointUnderAttack(point, team))
				attacked.push_back(threatened.back());
		}
		int teamSize = 0;
		int teamRank = 0;
		const int self = engine->IndexOfEdict(m_pEdict);
		for (int client = 1; client <= gpGlobals->maxClients; ++client)
		{
			edict_t* pClient = INDEXENT(client);
			IPlayerInfo* pInfo = pClient != nullptr && !pClient->IsFree() && playerinfomanager != nullptr ?
				playerinfomanager->GetPlayerInfo(pClient) : nullptr;
			if (pInfo == nullptr || !pInfo->IsConnected() || pInfo->GetTeamIndex() != team)
				continue;
			if (client < self)
				++teamRank;
			++teamSize;
		}
		// Alarm: the enemy is taking one of our points, everybody goes to the flag.
		if (!attacked.empty())
		{
			tiers[NAVGOAL_DEFEND] = attacked;
			Msg("[RCBOT2] goal-roam defend-alarm userid=%d team=%d entity=%d\n", userId, team,
				attacked.front().entity);
		}
		else if (!threatened.empty() && total > 0 && teamSize > 0 &&
			(teamRank + 0.5f) / teamSize < static_cast<float>(owned) / total)
		{
			std::sort(threatened.begin(), threatened.end(),
				[](const EmpiresNavGoalCandidate& a, const EmpiresNavGoalCandidate& b)
				{ return a.distance < b.distance; });
			EmpiresNavGoalCandidate guard = threatened.front();
			// A Grenadier with mines left first goes to the flag itself to lay them.
			int classId = -1;
			bool classDesired = false;
			bool classDesiredKnown = false;
			const char* classProperty = nullptr;
			const bool grenadier = readEmpiresClass(m_pEdict, classId, classDesired, classDesiredKnown,
				classProperty) && classId == EMP_CLASS_GRENADIER;
			const bool layMines = grenadier && countOwnEmpiresMines() < 8 && now >= m_flEmpiresDefendMineNext;
			Vector spot;
			if (!layMines && pickEmpiresDefendSpot(guard.origin, (m_iEmpiresNavGoalPicks + teamRank) % 2 == 0, spot))
			{
				Msg("[RCBOT2] goal-roam defend-spot userid=%d entity=%d flag=(%.0f %.0f %.0f) spot=(%.0f %.0f %.0f) dist=%.0f dz=%.0f\n",
					userId, guard.entity, guard.origin.x, guard.origin.y, guard.origin.z, spot.x, spot.y, spot.z,
					(spot - guard.origin).Length2D(), spot.z - guard.origin.z);
				guard.origin = spot;
				guard.distance = empiresNavGoalDistance2D(origin, spot);
			}
			tiers[NAVGOAL_DEFEND].push_back(guard);
		}
	}

	// 2-4. Resource points, enemy buildings, enemy commander from the entity list.
	const char* enemyCommanderClass = team == EMP_TEAM_NF ? "emp_imp_commander" : "emp_nf_commander";
	// Engineers look after their team's buildings first: damaged ones and ones still
	// under construction (both have health below max). At the building the existing
	// nearby repair (Engineer support, 400 window) takes over.
	int ownClassId = -1;
	bool ownClassDesired = false;
	bool ownClassDesiredKnown = false;
	const char* ownClassProperty = nullptr;
	const bool engineer = readEmpiresClass(m_pEdict, ownClassId, ownClassDesired, ownClassDesiredKnown,
		ownClassProperty) && ownClassId == EMP_CLASS_ENGINEER;
	std::vector<Vector> ownRefineries;
	for (int index = gpGlobals->maxClients + 1; index < gpGlobals->maxEntities; ++index)
	{
		edict_t* pEdict = INDEXENT(index);
		if (pEdict == nullptr || pEdict->IsFree() || pEdict->GetIServerEntity() == nullptr)
			continue;
		const char* className = pEdict->GetClassName();
		if (className == nullptr)
			continue;

		int type = -1;
		if (std::strcmp(className, "emp_resource_point") == 0)
			type = NAVGOAL_RESOURCE;
		else if (std::strcmp(className, enemyCommanderClass) == 0)
			type = NAVGOAL_COMMANDER;
		else if (std::strncmp(className, "emp_building_", 13) == 0 &&
			std::strstr(className, "physbox") == nullptr)
		{
			int buildingTeam = -1;
			if (!readEmpiresEntityIntProperty(pEdict, "m_iTeamNum", buildingTeam) ||
				(buildingTeam != EMP_TEAM_NF && buildingTeam != EMP_TEAM_BE))
			{
				buildingTeam = std::strncmp(className, "emp_building_nf_", 16) == 0 ? EMP_TEAM_NF :
					(std::strncmp(className, "emp_building_imp_", 17) == 0 ? EMP_TEAM_BE : -1);
			}
			if (buildingTeam == team && std::strstr(className, "refinery") != nullptr)
				ownRefineries.push_back(CBotGlobals::entityOrigin(pEdict));
			if (buildingTeam == enemyTeam)
				type = NAVGOAL_BUILDING;
			int health = 0, maxHealth = 0;
			if (engineer && buildingTeam == team &&
				isEmpiresRepairableBuildingClass(className, team) &&
				readEmpiresEntityIntProperty(pEdict, "m_iHealth", health) &&
				readEmpiresEntityIntProperty(pEdict, "m_iMaxHealth", maxHealth) &&
				health > 0 && health < maxHealth)
				type = NAVGOAL_REPAIR;
		}
		if (type < 0 || avoided(index))
			continue;
		Vector goal = CBotGlobals::entityOrigin(pEdict);
		if (type == NAVGOAL_REPAIR)
		{
			// The kit reaches 100: stand right at the building, on its floor, on the
			// side facing us (as_escort turret 292: routes to its origin ended one
			// storey up, 300-430 away).
			const ICollideable* collision = pEdict->GetCollideable();
			const Vector base = collision != nullptr ? collision->GetCollisionOrigin() : goal;
			// Outside the box in every direction: half diagonal plus a player's width.
			float reach = 40.0f;
			if (collision != nullptr)
			{
				const float hx = std::max(std::fabs(collision->OBBMins().x), std::fabs(collision->OBBMaxs().x));
				const float hy = std::max(std::fabs(collision->OBBMins().y), std::fabs(collision->OBBMaxs().y));
				reach += std::sqrt(hx * hx + hy * hy);
			}
			const float baseYaw = std::atan2(origin.y - base.y, origin.x - base.x);
			bool found = false;
			for (int step = 0; step < 8 && !found; ++step)
			{
				const float yaw = baseYaw + static_cast<float>((step + 1) / 2) * (step % 2 ? 1.0f : -1.0f) *
					static_cast<float>(M_PI) / 4.0f;
				const Vector wanted = base + Vector(std::cos(yaw), std::sin(yaw), 0.0f) * reach;
				Vector onMesh;
				if (RCBotNavMesh::ClosestPointOnMesh(Vector(wanted.x, wanted.y, base.z + 16.0f), 64.0f, onMesh) &&
					std::fabs(onMesh.z - base.z) < 60.0f)
				{
					goal = onMesh;
					found = true;
				}
			}
			if (!found)
				continue;
		}
		tiers[type].push_back({ index, goal, empiresNavGoalDistance2D(origin, goal), className });
	}

	// A resource point counts as ours when one of our refineries stands on it.
	std::vector<EmpiresNavGoalCandidate>& resources = tiers[NAVGOAL_RESOURCE];
	resources.erase(std::remove_if(resources.begin(), resources.end(),
		[&](const EmpiresNavGoalCandidate& candidate)
		{
			for (const Vector& refinery : ownRefineries)
				if (empiresNavGoalDistance2D(refinery, candidate.origin) < 300.0f)
					return true;
			return false;
		}), resources.end());

	// Nothing inside the circle of a turret that shot us: wait outside instead.
	// Routes go around the circle (SetTeamDangerCircle); no route means no goal.
	for (std::vector<EmpiresNavGoalCandidate>& list : tiers)
		list.erase(std::remove_if(list.begin(), list.end(),
			[&](const EmpiresNavGoalCandidate& candidate) { return isInsideEmpiresTurretThreat(candidate.origin); }),
			list.end());

	int tier = 0;
	while (tier < NAVGOAL_TYPES && tiers[tier].empty())
		++tier;
	if (tier >= NAVGOAL_TYPES)
	{
		m_flEmpiresNavGoalNextPick = now + kNavGoalNoneRetry;
		Msg("[RCBOT2] goal-roam none userid=%d team=%d reason=no-candidates\n", userId, team);
		return false;
	}

	// Spread teammates: rank of this bot among living team members by index.
	int rank = 0;
	const int selfIndex = engine->IndexOfEdict(m_pEdict);
	for (int client = 1; client < selfIndex && client <= gpGlobals->maxClients; ++client)
	{
		edict_t* pClient = INDEXENT(client);
		IPlayerInfo* pInfo = pClient != nullptr && !pClient->IsFree() && playerinfomanager != nullptr ?
			playerinfomanager->GetPlayerInfo(pClient) : nullptr;
		if (pInfo != nullptr && pInfo->IsConnected() && pInfo->GetTeamIndex() == team)
			++rank;
	}

	std::vector<EmpiresNavGoalCandidate>& candidates = tiers[tier];
	std::sort(candidates.begin(), candidates.end(),
		[](const EmpiresNavGoalCandidate& a, const EmpiresNavGoalCandidate& b)
		{
			return a.distance < b.distance;
		});
	const EmpiresNavGoalCandidate& choice =
		candidates[(static_cast<unsigned>(rank) + m_iEmpiresNavGoalPicks) % candidates.size()];
	++m_iEmpiresNavGoalPicks;

	if (!requestNavigation(choice.origin, RCBotNavigation::NavigationPolicy::NAVMESH_ONLY,
		choice.entity))
	{
		m_flEmpiresNavGoalNextPick = now + kNavGoalNoneRetry;
		Msg("[RCBOT2] goal-roam none userid=%d team=%d reason=request-rejected\n", userId, team);
		return false;
	}

	m_bEmpiresNavGoalActive = true;
	m_iEmpiresNavGoalType = tier;
	m_iEmpiresNavGoalEntity = choice.entity;
	std::snprintf(m_szEmpiresNavGoalClass, sizeof(m_szEmpiresNavGoalClass), "%s", choice.className);
	m_vEmpiresNavGoalOrigin = choice.origin;
	m_flEmpiresNavGoalStart = now;
	// Generous: bots run ~190 u/s; allow detours and short fights.
	m_flEmpiresNavGoalDeadline = now + 30.0f + choice.distance / 100.0f;
	m_iEmpiresNavGoalRetries = 0;
	m_iEmpiresNavGoalAttempts = 0;
	m_uEmpiresNavGoalBlockMark = RCBotNavMesh::BlockGeneration();
	Msg("[RCBOT2] goal-roam pick userid=%d team=%d type=%s entity=%d class=%s pos=(%.0f %.0f %.0f) dist=%.0f rank=%d candidates=%d deadline=%.0f\n",
		userId, team, empiresNavGoalTypeName(tier), choice.entity, choice.className,
		choice.origin.x, choice.origin.y, choice.origin.z, choice.distance, rank,
		static_cast<int>(candidates.size()), m_flEmpiresNavGoalDeadline - now);
	return true;
}

int CBotEmpires::countOwnEmpiresMines(std::vector<Vector>* positions)
{
	const int team = getTeam();
	const char* expected = team == EMP_TEAM_NF ? "emp_nf_mine_grenade_projectile" :
		(team == EMP_TEAM_BE ? "emp_imp_mine_grenade_projectile" : nullptr);
	if (expected == nullptr || m_pEdict == nullptr)
		return 0;
	const int self = engine->IndexOfEdict(m_pEdict);
	int count = 0;
	for (int index = gpGlobals->maxClients + 1; index < gpGlobals->maxEntities; ++index)
	{
		edict_t* pMine = INDEXENT(index);
		if (pMine == nullptr || pMine->IsFree() || pMine->GetClassName() == nullptr ||
			std::strcmp(pMine->GetClassName(), expected) != 0)
			continue;
		int owner = -1;
		if (!readEmpiresWeaponHandleProperty(pMine, "m_hThrower", owner) || owner <= 0)
			readEmpiresWeaponHandleProperty(pMine, "m_hOwnerEntity", owner);
		if (owner != self)
			continue;
		++count;
		if (positions != nullptr)
			positions->push_back(CBotGlobals::entityOrigin(pMine));
	}
	return count;
}

// Grenadier on a defended point lays mines in a ring (vehicles come for the
// flag). The game allows 8 live mines per player; a 9th detonates the oldest,
// so the bot stops at 8 wherever they lie.
void CBotEmpires::handleEmpiresDefendMines(const float now)
{
	constexpr int kMaxMines = 8;
	constexpr float kRingRadius = 250.0f;
	if (isEmpiresMineActionActive() || now < m_flEmpiresDefendMineNext)
		return;
	if (empiresNavGoalDistance2D(getOrigin(), m_vEmpiresDefendHoldOrigin) > 400.0f)
		return; // holding a spread spot, too far to throw onto the flag ring
	int classId = -1;
	bool classDesired = false;
	bool classDesiredKnown = false;
	const char* classProperty = nullptr;
	if (!readEmpiresClass(m_pEdict, classId, classDesired, classDesiredKnown, classProperty) ||
		classId != EMP_CLASS_GRENADIER)
	{
		m_flEmpiresDefendMineNext = now + 30.0f;
		return;
	}
	const int userId = engine->GetPlayerUserId(m_pEdict);
	std::vector<Vector> mines;
	const int own = countOwnEmpiresMines(&mines);
	if (own >= kMaxMines)
	{
		if (!m_bEmpiresDefendMineFullLogged)
		{
			m_bEmpiresDefendMineFullLogged = true;
			std::string list;
			for (const Vector& mine : mines)
				list += " " + std::to_string(static_cast<int>((mine - m_vEmpiresDefendHoldOrigin).Length2D()));
			Msg("[RCBOT2] defend-mine full userid=%d flag=%d own=%d dist_to_flag=%s\n",
				userId, m_iEmpiresDefendHoldEntity, own, list.c_str());
		}
		m_flEmpiresDefendMineNext = now + 5.0f;
		return;
	}
	m_bEmpiresDefendMineFullLogged = false;

	const float angle = static_cast<float>(own) * (2.0f * static_cast<float>(M_PI) / kMaxMines);
	const Vector aim = m_vEmpiresDefendHoldOrigin +
		Vector(std::cos(angle) * kRingRadius, std::sin(angle) * kRingRadius, 0.0f);
	setLookVector(aim);
	setLookAtTask(LOOK_VECTOR, 1.0f);
	if (m_flEmpiresDefendMineAimUntil <= 0.0f)
	{
		m_flEmpiresDefendMineAimUntil = now + 0.8f; // turn toward the slot first
		return;
	}
	if (now < m_flEmpiresDefendMineAimUntil)
		return;
	m_flEmpiresDefendMineAimUntil = 0.0f;
	if (requestEmpiresMineFireOnce())
	{
		m_flEmpiresDefendMineNext = now + 1.5f;
		Msg("[RCBOT2] defend-mine throw userid=%d flag=%d slot=%d own_before=%d aim=(%.0f %.0f %.0f)\n",
			userId, m_iEmpiresDefendHoldEntity, own, own, aim.x, aim.y, aim.z);
	}
	else
	{
		m_flEmpiresDefendMineNext = now + 15.0f;
		Msg("[RCBOT2] defend-mine unavailable userid=%d flag=%d own=%d\n",
			userId, m_iEmpiresDefendHoldEntity, own);
	}
}

namespace
{
// button -> (userid, until in curtime); cleared on map change
std::vector<std::pair<int, std::pair<int, float>>> s_doorButtonClaims;
}

// A map button that opens a closed door nearby: walk to it, look at it, press use once.
// as_escort: BE spawns behind gate door1; 60 s after spawn button control1 unlocks and
// toggles it. One bot per button; the door leaving its closed rest ends the job.
bool CBotEmpires::handleEmpiresDoorButton()
{
	if (gpGlobals == nullptr || engine == nullptr || m_pSchedules == nullptr ||
		m_pEdict == nullptr || m_pEdict->IsFree())
		return false;
	auto& s_claims = s_doorButtonClaims;
	const float now = gpGlobals->curtime;
	const int team = getTeam();
	const int userId = engine->GetPlayerUserId(m_pEdict);
	const Vector origin = getOrigin();
	auto claimOf = [&](int button) -> std::pair<int, float>* {
		for (auto& claim : s_claims)
			if (claim.first == button)
				return &claim.second;
		s_claims.emplace_back(button, std::make_pair(-1, 0.0f));
		return &s_claims.back().second;
	};
	auto finish = [&](const char* reason, float retry) {
		Msg("[RCBOT2] door-button end userid=%d button=%d door=%d reason=%s bot=(%.0f %.0f %.0f)\n",
			userId, m_iEmpiresDoorButton, m_iEmpiresDoorButtonDoor, reason, origin.x, origin.y, origin.z);
		std::pair<int, float>* claim = claimOf(m_iEmpiresDoorButton);
		if (claim->first == userId)
			claim->second = 0.0f;
		m_iEmpiresDoorButton = -1;
		m_flEmpiresDoorButtonNext = now + retry;
		if (m_bEmpiresDoorButtonMoving)
			cancelNavigationAttempt("door-button-end");
		m_bEmpiresDoorButtonMoving = false;
	};

	RCBotNavMeshProvider::DoorButton entry;
	if (m_iEmpiresDoorButton < 0)
	{
		if (now < m_flEmpiresDoorButtonNext)
			return false;
		m_flEmpiresDoorButtonNext = now + 2.0f;
		if (!RCBotNavMeshProvider::FindDoorButton(team, origin, 3000.0f, entry))
			return false;
		std::pair<int, float>* claim = claimOf(entry.button);
		if (claim->first != userId && claim->second > now)
			return false;
		Vector stand;
		RCBotNavMesh::Path path;
		if (!RCBotNavMesh::ClosestPointOnMesh(entry.origin, 128.0f, stand) ||
			RCBotNavMesh::BuildPath(origin, stand, path, this) != RCBotNavMesh::PathResult::SUCCESS)
		{
			m_flEmpiresDoorButtonNext = now + 10.0f;
			return false;
		}
		*claim = std::make_pair(userId, now + 95.0f);
		m_iEmpiresDoorButton = entry.button;
		m_iEmpiresDoorButtonDoor = entry.door;
		m_vEmpiresDoorButtonOrigin = entry.origin;
		// A locked button may unlock on a map timer; wait at it for a while.
		m_flEmpiresDoorButtonUntil = now + 90.0f;
		m_flEmpiresDoorButtonLastUse = 0.0f;
		m_bEmpiresDoorButtonMoving = false;
		Msg("[RCBOT2] door-button start userid=%d team=%d button=%d door=%d at=(%.0f %.0f %.0f) stand=(%.0f %.0f %.0f) dist=%.0f\n",
			userId, team, entry.button, entry.door, entry.origin.x, entry.origin.y, entry.origin.z,
			stand.x, stand.y, stand.z, (entry.origin - origin).Length2D());
	}
	entry.button = m_iEmpiresDoorButton;
	entry.door = m_iEmpiresDoorButtonDoor;
	entry.origin = m_vEmpiresDoorButtonOrigin;
	if (!RCBotNavMeshProvider::DoorButtonPressable(entry))
	{
		finish(m_flEmpiresDoorButtonLastUse > 0.0f ? "door-moving" : "not-pressable", 5.0f);
		return false;
	}
	if (now > m_flEmpiresDoorButtonUntil)
	{
		finish(m_flEmpiresDoorButtonLastUse > 0.0f ? "pressed-no-effect" :
			RCBotNavMeshProvider::DoorButtonLocked(entry) ? "still-locked" : "timeout", 120.0f);
		return false;
	}

	const Vector button = m_vEmpiresDoorButtonOrigin;
	if ((button - origin).Length2D() > 72.0f)
	{
		if (m_bEmpiresDoorButtonMoving && m_pSchedules->isEmpty())
		{
			finish("arrived-too-far", 30.0f);
			return false;
		}
		if (!m_bEmpiresDoorButtonMoving)
		{
			Vector stand = button;
			RCBotNavMesh::ClosestPointOnMesh(button, 128.0f, stand);
			if (m_bEmpiresNavGoalActive)
				endEmpiresNavmeshGoal("drop", "door-button", 0.0f);
			m_pSchedules->freeMemory();
			if (!requestNavigation(stand, RCBotNavigation::NavigationPolicy::NAVMESH_ONLY, 0))
			{
				finish("no-navigation", 30.0f);
				return false;
			}
			m_bEmpiresDoorButtonMoving = true;
		}
		return true;
	}

	if (m_bEmpiresDoorButtonMoving)
	{
		cancelNavigationAttempt("door-button-arrived");
		m_pSchedules->freeMemory();
		m_bEmpiresDoorButtonMoving = false;
	}
	stopMoving();
	setLookVector(button);
	setLookAtTask(LOOK_VECTOR, 1.0f);
	if (RCBotNavMeshProvider::DoorButtonLocked(entry))
		return true;
	if (isFacing(button) && now - m_flEmpiresDoorButtonLastUse > 1.5f)
	{
		// Held like the engineer crate use; a one-frame tap did nothing (19 presses, door stayed).
		m_pButtons->holdButton(IN_USE, 0.0f, 0.25f, 0.0f);
		m_flEmpiresDoorButtonLastUse = now;
		const QAngle eyeAngles = CBotGlobals::playerAngles(m_pEdict);
		QAngle want;
		VectorAngles(button - getEyePosition(), want);
		Msg("[RCBOT2] door-button press userid=%d button=%d door=%d bot=(%.0f %.0f %.0f) eye_dist=%.0f eye=(%.1f %.1f) want=(%.1f %.1f) lastcmd_buttons=0x%x\n",
			userId, m_iEmpiresDoorButton, m_iEmpiresDoorButtonDoor, origin.x, origin.y, origin.z,
			(button - getEyePosition()).Length(), eyeAngles.x, eyeAngles.y, want.x, want.y,
			m_pPlayerInfo != nullptr ? m_pPlayerInfo->GetLastUserCommand().buttons : -1);
	}
	return true;
}

void EmpiresObjectiveLevelReset()
{
	for (TeamTurretThreat& t : s_teamTurretThreat)
		t = TeamTurretThreat();
	s_doorButtonClaims.clear();
}
