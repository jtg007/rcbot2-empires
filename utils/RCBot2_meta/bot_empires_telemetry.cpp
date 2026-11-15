#include "bot_empires_telemetry.h"

#include "bot.h"
#include "bot_local_yielding.h"
#include "bot_globals.h"
#include "bot_empires_squad_runtime.h"
#include "bot_navigator.h"
#include "bot_navmesh_api.h"
#include "bot_cvars.h"
#include "bot_order_protocol_telemetry.h"
#include "bot_schedule.h"
#include "bot_squads.h"
#include "bot_task.h"
#include "bot_waypoint_locations.h"
#include "bot_weapons.h"
#include "cmodel.h"
#include "const.h"
#include "edict.h"
#include "engine/IEngineTrace.h"
#include "navmesh/util/UtilTrace.h"
#include "rcbot/logging.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <cstring>
#include <limits>
#include <map>
#include <set>
#include <string>

namespace
{
	constexpr double kStatePollSeconds = 0.25;
	constexpr double kSnapshotSeconds = 1.5;
	constexpr double kRetryWindowSeconds = 10.0;
	constexpr float kGoalChangeDistance = 24.0f;
	constexpr float kDestinationReachDistance = 128.0f;
	constexpr float kStuckMovementEpsilon = 8.0f;
	constexpr int kStuckSamples = 5;
	constexpr double kLocalConflictPollSeconds = 0.25;
	constexpr float kLocalConflictLookAhead = 96.0f;
	constexpr float kLocalConflictHullPadding = 12.0f;
	constexpr float kLocalConflictProgressEpsilon = 4.0f;
	constexpr std::uint32_t kPrecisionLadder = 1u << 0;
	constexpr std::uint32_t kPrecisionJump = 1u << 1;
	constexpr std::uint32_t kPrecisionDrop = 1u << 2;
	constexpr std::uint32_t kPrecisionDoor = 1u << 3;
	constexpr std::uint32_t kPrecisionLift = 1u << 4;
	constexpr std::uint32_t kPrecisionTeleport = 1u << 5;
	constexpr std::uint32_t kPrecisionVertical = 1u << 6;
	constexpr std::size_t kFlushBytes = 32 * 1024;
	constexpr const char* kPathId = "GAME";

	std::string jsonString(const char* value)
	{
		std::string result = "\"";
		if (value != nullptr)
		{
			for (const unsigned char* p = reinterpret_cast<const unsigned char*>(value);
				*p != '\0'; ++p)
			{
				switch (*p)
				{
				case '\\': result += "\\\\"; break;
				case '\"': result += "\\\""; break;
				case '\b': result += "\\b"; break;
				case '\f': result += "\\f"; break;
				case '\n': result += "\\n"; break;
				case '\r': result += "\\r"; break;
				case '\t': result += "\\t"; break;
				default:
					if (*p < 0x20)
					{
						char escaped[8] = {};
						snprintf(escaped, sizeof(escaped), "\\u%04x", *p);
						result += escaped;
					}
					else
						result += static_cast<char>(*p);
					break;
				}
			}
		}
		result += '"';
		return result;
	}

	std::string number(const double value)
	{
		char buffer[64] = {};
		snprintf(buffer, sizeof(buffer), "%.6f", value);
		return buffer;
	}

	std::string vectorJson(const Vector& value)
	{
		return "[" + number(value.x) + "," + number(value.y) + "," +
			number(value.z) + "]";
	}

	bool sameVector(const Vector& left, const Vector& right,
		const float tolerance = 0.01f)
	{
		return (left - right).LengthSqr() <= tolerance * tolerance;
	}

	std::string lowerCopy(const std::string& input)
	{
		std::string result = input;
		for (char& c : result)
		{
			if (c >= 'A' && c <= 'Z')
				c = static_cast<char>(c - 'A' + 'a');
		}
		return result;
	}

	bool contains(const std::string& value, const char* needle)
	{
		return needle != nullptr && value.find(needle) != std::string::npos;
	}

	const char* className(const int classId)
	{
		switch (classId)
		{
		case 0: return "scout";
		case 1: return "rifleman";
		case 2: return "grenadier";
		case 3: return "engineer";
		default: return "unknown";
		}
	}

	const char* teamName(const int team)
	{
		if (team == 2)
			return "NF";
		if (team == 3)
			return "BE";
		return "unknown";
	}

	float dot2d(const Vector& left, const Vector& right)
	{
		return left.x * right.x + left.y * right.y;
	}

	float cross2d(const Vector& left, const Vector& right)
	{
		return left.x * right.y - left.y * right.x;
	}

	float horizontalHullRadius(const CBot* bot)
	{
		if (bot == nullptr || bot->getEdict() == nullptr ||
			bot->getEdict()->GetCollideable() == nullptr)
			return 16.0f;
		const ICollideable* collideable = bot->getEdict()->GetCollideable();
		const Vector mins = collideable->OBBMins();
		const Vector maxs = collideable->OBBMaxs();
		return (std::max)(std::max(std::fabs(mins.x), std::fabs(maxs.x)),
			(std::max)(std::fabs(mins.y), std::fabs(maxs.y)));
	}

	bool worldHullClear(const CBot* bot, const Vector& from, const Vector& to)
	{
		if (bot == nullptr || bot->getEdict() == nullptr ||
			bot->getEdict()->GetCollideable() == nullptr || enginetrace == nullptr)
			return false;
		const ICollideable* collideable = bot->getEdict()->GetCollideable();
		Ray_t ray;
		ray.Init(from, to, collideable->OBBMins(), collideable->OBBMaxs());
		CTraceFilterWorldAndPropsOnly filter;
		trace_t trace{};
		enginetrace->TraceRay(ray, MASK_PLAYERSOLID, &filter, &trace);
		return !trace.startsolid && !trace.allsolid && trace.fraction >= 1.0f;
	}

	const char* navigationProviderName(
		const RCBotNavigation::Provider provider)
	{
		switch (provider)
		{
		case RCBotNavigation::Provider::WAYPOINT: return "WAYPOINT";
		case RCBotNavigation::Provider::NAVMESH: return "NAVMESH";
		case RCBotNavigation::Provider::LOCAL_EXPLORER: return "LOCAL_EXPLORER";
		case RCBotNavigation::Provider::BUILDING_RELATIVE:
			return "BUILDING_RELATIVE";
		case RCBotNavigation::Provider::UNKNOWN: return "UNKNOWN";
		}
		return "UNKNOWN";
	}

	const char* navigationResultName(const RCBotNavigation::Result result)
	{
		switch (result)
		{
		case RCBotNavigation::Result::COMPLETE: return "COMPLETE";
		case RCBotNavigation::Result::CANCELLED: return "CANCELLED";
		case RCBotNavigation::Result::PATH_FAILED: return "PATH_FAILED";
		case RCBotNavigation::Result::PATH_STUCK: return "PATH_STUCK";
		case RCBotNavigation::Result::UNREACHABLE: return "UNREACHABLE";
		case RCBotNavigation::Result::NONE: return "NONE";
		}
		return "NONE";
	}

	struct BotState
	{
		bool valid = false;
		int id = -1;
		int entityIndex = -1;
		int userId = -1;
		std::string name;
		int team = -1;
		int classId = -1;
		bool alive = false;
		int health = -1;
		Vector origin = vec3_origin;
		bool hasGoal = false;
		Vector goal = vec3_origin;
		int waypoint = -1;
		int goalWaypoint = -1;
		int routeSize = 0;
		bool destinationKnown = false;
		Vector destination = vec3_origin;
		int enemyIndex = -1;
		int enemyUserId = -1;
		bool hasEnemy = false;
		bool attacking = false;
		bool inSquad = false;
		bool squadLeader = false;
		std::string schedule;
		std::string task;
		std::string taskKey;
		std::string weapon;
		std::string roleState;
		float speed = 0.0f;
		Vector velocity = vec3_origin;
		Vector desiredDirection = vec3_origin;
		float desiredForward = 0.0f;
		float desiredSide = 0.0f;
		float desiredUp = 0.0f;
		bool wantsMovement = false;
		bool localYieldWaiting = false;
		bool navigationActive = false;
		std::uint64_t navigationIntentId = 0;
		std::uint64_t navigationAttemptId = 0;
		std::string navigationProvider;
		int currentSegment = -1;
		int nextSegment = -1;
		bool precisionContext = false;
		std::uint32_t precisionFlags = 0;
		int squadId = -1;
		int formationSlot = -1;
		bool spatialMovementActive = false;
		RCBotRecovery::RecoveryEpisodeIdentity recoveryEpisode;
	};

	struct BotRecord
	{
		bool observed = false;
		BotState last;
		double lastPoll = -1.0;
		double lastSnapshot = -1.0;
		int stagnantSamples = 0;
		bool stuckReported = false;
		int life = 0;
		int pathRequest = 0;
		int lastPathWaypoint = -1;
		double lastPathTime = -1.0;
		int commanderOrder = 0;
		bool counted = false;
		bool hasLastNavigationKey = false;
		RCBotNavigationExperience::Key lastNavigationKey;
		std::uint64_t lastNavigationAtMs = 0;
		std::uint64_t activeNavigationAttemptId = 0;
		std::array<std::uint64_t, 3> pendingNavigationSignals{};
		std::uint64_t activeTransitionAttemptId = 0;
		std::int64_t activeTransitionFrom = -1;
		std::int64_t activeTransitionTo = -1;
		RCBotNavigationExperience::Traversal activeTransitionTraversal =
			RCBotNavigationExperience::Traversal::UNKNOWN;
		bool activeTransitionStarted = false;
		bool hasLastLocalConflictProgress = false;
		float lastLocalConflictRemainingDistance = -1.0f;
		std::uint64_t lastRecoveryProgressEpisodeId = 0;
		double lastRecoveryProgressAt = -1.0;
	};

	class CEmpiresAnalysisRecorder
	{
	public:
		bool isActive() const { return m_active; }

		bool start(const char* mapName)
		{
			if (m_active)
			{
				setError("analysis session is already active");
				return false;
			}
			if (filesystem == nullptr)
			{
				setError("filesystem interface is unavailable");
				return false;
			}

			resetSession();
			m_map = mapName != nullptr && *mapName != '\0' ? mapName : "unknown";
			m_navigationExperience.reset(m_map, navigationDataIdentity());
			m_sessionId = makeSessionId();
			m_relativePath = "analysis/session-" + m_sessionId;
			for (int suffix = 1; suffix < 100 &&
				filesystem->FileExists((m_relativePath + "/metadata.json").c_str(), kPathId);
				++suffix)
			{
				m_relativePath = "analysis/session-" + m_sessionId + "-" +
					std::to_string(suffix);
			}

			filesystem->CreateDirHierarchy(m_relativePath.c_str(), kPathId);
			m_events = filesystem->Open((m_relativePath + "/events.jsonl").c_str(),
				"wb", kPathId);
			m_snapshots = filesystem->Open((m_relativePath + "/snapshots.jsonl").c_str(),
				"wb", kPathId);
			if (m_events == FILESYSTEM_INVALID_HANDLE ||
				m_snapshots == FILESYSTEM_INVALID_HANDLE)
			{
				closeFiles();
				setError("could not open session JSONL files");
				return false;
			}

			m_start = std::chrono::steady_clock::now();
			m_active = true;
			m_metadata =
				"{\n"
				"  \"recorder_version\": \"1\",\n"
				"  \"session_id\": " + jsonString(m_sessionId.c_str()) + ",\n"
				"  \"map\": " + jsonString(m_map.c_str()) + ",\n"
				"  \"baseline_commit\": \"fbda92d\",\n"
				"  \"baseline_tag\": \"empires-known-good-20260909\",\n"
				"  \"snapshot_interval_seconds\": 1.5,\n"
				"  \"state_poll_interval_seconds\": 0.25,\n"
				"  \"local_avoidance_gate\": \"LOCAL_AVOIDANCE_SIGNAL_READY\",\n"
				"  \"local_yielding_enabled\": " +
				std::string(eta_local_yielding.GetBool() ? "true" : "false") + ",\n"
				"  \"local_yield_sticky_seconds\": " + number(
					RCBotLocalYielding::Policy().stickySeconds) + ",\n"
				"  \"local_yield_max_wait_seconds\": " + number(
					RCBotLocalYielding::Policy().maxWaitSeconds) + ",\n"
				"  \"local_avoidance_behavior_changes\": 0,\n"
				"  \"started_unix\": " + std::to_string(static_cast<long long>(std::time(nullptr))) + "\n"
				"}\n";
			if (!writeWholeFile(m_relativePath + "/metadata.json", m_metadata))
			{
				disable("could not write metadata.json");
				return false;
			}
			m_lastError.clear();
			return true;
		}

		bool stop()
		{
			if (!m_active)
			{
				setError("analysis session is not active");
				return false;
			}

			const auto ended = m_localConflictTracker.EndAll(
				elapsed(), RCBotLocalAvoidance::Lifecycle::ENDED_BY_LIFECYCLE);
			for (const auto& update : ended)
				handleLocalConflictUpdate(update);
			RCBotLocalYielding::Reset();
			m_duration = elapsed();
			if (!flush())
			{
				disable("could not flush session JSONL files");
				return false;
			}
			closeFiles();
			m_active = false;
			m_navigationExperience.setLearnedNavigationBehaviorChanges(
				m_learnedNavigationBehaviorChanges);
			m_navigationExperience.setLearnedNavigationMetrics(
				m_learnedEdgesWithPenalty, m_maxObservedPenalty,
				averageObservedPenalty(), eta_nav_learning.GetBool());
			const bool experienceOkay = writeWholeFile(
				m_relativePath + "/navigation_experience.json",
				m_navigationExperience.canonicalJson() + "\n");
			const bool summaryOkay = writeWholeFile(m_relativePath + "/summary.json",
				makeSummary());
			if (!experienceOkay)
				setError("could not write navigation_experience.json");
			if (!summaryOkay)
				setError("could not write summary.json");
			return summaryOkay && experienceOkay;
		}

		void mark(const char* text)
		{
			if (!m_active)
			{
				setError("cannot mark: analysis session is not active");
				return;
			}
			const double sessionTime = elapsed();
			std::string line = "{\"session_time\":" + number(sessionTime) +
				",\"game_time\":" + gameTime() +
				",\"event\":\"USER_MARK\",\"text\":" + jsonString(text) + "}\n";
			append(m_eventBuffer, line, true);
			++m_eventCount;
			++m_eventCounts["USER_MARK"];
			++m_userMarks;
		}

		void recordBotEvent(CBot* bot, const char* event,
			const edict_t* target, const char* detail)
		{
			if (!m_active || !validBot(bot) || event == nullptr)
				return;
			BotState state;
			if (!readState(bot, state))
				return;
			writeEvent(event, state, target, detail, -1, nullptr);

			// BOT_DEATH is emitted synchronously by CBotEmpires::died(). Keep the
			// sampled state in step with that exact hook so the next dead-state
			// poll does not emit a duplicate death, while the later confirmed
			// alive transition can still be classified as BOT_RESPAWN.
			if (std::strcmp(event, "BOT_DEATH") == 0)
			{
				BotRecord& record = m_bots[state.id];
				if (record.observed)
					record.last = state;
			}
		}

		void recordPathEvent(CBot* bot, const char* event,
			const int targetWaypoint, const char* detail)
		{
			if (!m_active || !validBot(bot))
				return;
			BotState state;
			if (!readState(bot, state))
				return;
			BotRecord& record = m_bots[state.id];
			const double now = elapsed();
			int requestId = record.pathRequest;
			if (event != nullptr && std::string(event) == "PATH_REQUEST")
			{
				++requestId;
				record.pathRequest = requestId;
				if (record.lastPathTime >= 0.0 &&
					now - record.lastPathTime <= kRetryWindowSeconds &&
					targetWaypoint == record.lastPathWaypoint)
				{
					writeEvent("RETRY", state, nullptr, "same-target-path-request", targetWaypoint,
						nullptr);
				}
				record.lastPathTime = now;
				record.lastPathWaypoint = targetWaypoint;
			}
			std::string pathDetail = detail != nullptr ? detail : "";
			if (!pathDetail.empty())
				pathDetail += ";";
			pathDetail += "path_request_id=" + std::to_string(requestId);
			writeEvent(event, state, nullptr, pathDetail.c_str(), targetWaypoint, nullptr);
			if (event != nullptr &&
				(std::strcmp(event, "NAV_PROGRESS") == 0 ||
					std::strcmp(event, "NAV_NO_PROGRESS") == 0))
			{
				const auto signal = std::strcmp(event, "NAV_PROGRESS") == 0 ?
					RCBotLocalAvoidance::CorrelationSignal::NAV_PROGRESS :
					RCBotLocalAvoidance::CorrelationSignal::NAV_NO_PROGRESS;
				correlateLocalConflicts(state, state.navigationIntentId,
					state.navigationAttemptId, signal);
			}
		}

		void recordRecoveryEpisodeEvent(CBot* bot, const char* event,
			const RCBotRecovery::RecoveryEpisodeIdentity& identity,
			const char* reason)
		{
			if (!m_active || !validBot(bot) || event == nullptr ||
				!identity.IsActive())
				return;
			BotState state;
			if (!readState(bot, state))
				return;
			state.recoveryEpisode = identity;
			writeEvent(event, state, nullptr, reason, -1, nullptr, nullptr, 0,
				&identity, nullptr, RCBotNavigation::Result::NONE);
		}

		void recordRecoveryEpisodeProgress(CBot* bot, const char* reason)
		{
			if (!m_active || !validBot(bot))
				return;
			BotState state;
			if (!readState(bot, state) || !state.recoveryEpisode.IsActive())
				return;
			BotRecord& record = m_bots[state.id];
			const double now = elapsed();
			if (record.lastRecoveryProgressEpisodeId ==
				state.recoveryEpisode.episodeId &&
				record.lastRecoveryProgressAt >= 0.0 &&
				now - record.lastRecoveryProgressAt < 1.0)
				return;
			record.lastRecoveryProgressEpisodeId =
				state.recoveryEpisode.episodeId;
			record.lastRecoveryProgressAt = now;
			writeEvent("RECOVERY_EPISODE_PROGRESS", state, nullptr, reason, -1,
				nullptr, nullptr, 0, &state.recoveryEpisode, nullptr,
				RCBotNavigation::Result::NONE);
		}

		void recordRecoveryPolicyEvent(CBot* bot, const char* event,
			const RCBotRecovery::RecoveryEpisodeIdentity& identity,
			const int consecutiveExhaustions, const char* reason)
		{
			if (!m_active || !validBot(bot) || event == nullptr ||
				!identity.IsActive())
				return;
			BotState state;
			if (!readState(bot, state))
				return;
			state.recoveryEpisode = identity;
			writeEvent(event, state, nullptr, reason, -1, nullptr, nullptr, 0,
				&identity, nullptr, RCBotNavigation::Result::NONE,
				consecutiveExhaustions);
			if (std::strcmp(event, "RECOVERY_SUPPRESSION_APPLIED") == 0)
				++m_recoveryEpisodeBehaviorChanges;
		}

		void recordNavigationAttemptStarted(CBot* bot,
			const RCBotNavigation::NavigationIntent& intent,
			const RCBotNavigation::NavigationAttempt& attempt)
		{
			if (!m_active || !validBot(bot) || !attempt.IsActive())
				return;
			BotState state;
			if (!readState(bot, state))
				return;
			BotRecord& record = m_bots[state.id];
			const RCBotNavigationExperience::Key key = experienceKey(
				state, intent, attempt, false);
			record.hasLastNavigationKey = true;
			record.lastNavigationKey = key;
			record.lastNavigationAtMs = sessionMilliseconds();
			record.activeNavigationAttemptId = attempt.GetAttemptId();
			record.activeTransitionAttemptId = attempt.GetAttemptId();
			record.activeTransitionFrom = -1;
			record.activeTransitionTo = -1;
			record.activeTransitionTraversal =
				RCBotNavigationExperience::Traversal::UNKNOWN;
			record.activeTransitionStarted = false;
			record.pendingNavigationSignals.fill(0);
			const std::string detail = experienceKeyDetail(key,
				attempt.GetAttemptId(), "STARTED");
			writeEvent("NAV_EXPERIENCE_ATTEMPT_STARTED", state, nullptr,
				detail.c_str(), intent.goalId, nullptr, nullptr, 0,
				&intent.recoveryEpisode,
				&intent, RCBotNavigation::Result::NONE);
		}

		void recordNavigationAttemptTerminal(CBot* bot,
			const RCBotNavigation::NavigationIntent& intent,
			const RCBotNavigation::NavigationAttempt& attempt,
			const RCBotNavigation::Result result, const char* reason)
		{
			if (!m_active || !validBot(bot) || attempt.GetAttemptId() == 0)
				return;
			BotState state;
			if (!readState(bot, state))
				return;
			if (result == RCBotNavigation::Result::PATH_STUCK)
				correlateLocalConflicts(state, intent.id, attempt.GetAttemptId(),
					RCBotLocalAvoidance::CorrelationSignal::PATH_STUCK);
			const RCBotNavigationExperience::Key key = experienceKey(
				state, intent, attempt, true);
			RCBotNavigationExperience::Observation observation;
			observation.key = key;
			observation.botId = state.id;
			observation.attemptId = attempt.GetAttemptId();
			observation.timestampMs = sessionMilliseconds();
			observation.result = experienceResult(result, reason);
			observation.traversalSeconds = (std::max)(0.0,
				static_cast<double>(attempt.GetEndTime()) -
				static_cast<double>(attempt.GetStartTime()));
			observation.progressUnits = attempt.GetProgressCount();
			if (!m_navigationExperience.record(observation))
				return;
			const RCBotNavigationExperience::TransitionResult transitionResult =
				transitionResultFor(result);
			RCBotNavigationExperience::CancellationKind cancellation =
				RCBotNavigationExperience::CancellationKind::NONE;
			if (transitionResult ==
				RCBotNavigationExperience::TransitionResult::CANCELLED)
			{
				const RCBotNavigationExperience::Result cancellationResult =
					experienceResult(result, reason);
				cancellation = cancellationResult ==
					RCBotNavigationExperience::Result::CANCELLED_REPLACEMENT ?
						RCBotNavigationExperience::CancellationKind::REPLACEMENT :
					cancellationResult ==
						RCBotNavigationExperience::Result::CANCELLED_LIFECYCLE ?
							RCBotNavigationExperience::CancellationKind::LIFECYCLE :
							RCBotNavigationExperience::CancellationKind::EXTERNAL;
			}
		const RCBotNavigation::SegmentKey& segment = attempt.GetKey().segment;
		BotRecord& record = m_bots[state.id];
		std::int64_t transitionFrom = segment.currentWaypoint;
		std::int64_t transitionTo = segment.nextWaypoint;
		RCBotNavigationExperience::Traversal transitionTraversal =
			transitionTraversalFor(intent.provider, transitionFrom, transitionTo);
		if (transitionResult ==
			RCBotNavigationExperience::TransitionResult::CANCELLED)
		{
			const bool started = record.activeTransitionStarted &&
				record.activeTransitionAttemptId == attempt.GetAttemptId() &&
				record.activeTransitionFrom == transitionFrom &&
				record.activeTransitionTo == transitionTo;
			if (started)
				transitionTraversal = record.activeTransitionTraversal;
			else
			{
				transitionFrom = -1;
				transitionTo = -1;
				transitionTraversal =
					RCBotNavigationExperience::Traversal::UNKNOWN;
			}
		}
		if (result != RCBotNavigation::Result::COMPLETE ||
			intent.provider == RCBotNavigation::Provider::WAYPOINT)
		{
			recordNavigationTransition(bot, intent, attempt,
				transitionFrom, transitionTo,
				transitionTraversal, transitionResult, cancellation, reason, state);
		}
		else if (intent.provider ==
			RCBotNavigation::Provider::LOCAL_EXPLORER)
		{
			recordNavigationTransition(bot, intent, attempt, -1, -1,
				RCBotNavigationExperience::Traversal::UNKNOWN,
				RCBotNavigationExperience::TransitionResult::UNKNOWN,
				RCBotNavigationExperience::CancellationKind::NONE,
				"local-explorer-has-no-stable-edge-identity", state);
		}
			record.hasLastNavigationKey = true;
			record.lastNavigationKey = key;
			record.lastNavigationAtMs = observation.timestampMs;
			record.activeNavigationAttemptId = 0;
			for (std::size_t signal = 0;
				signal < record.pendingNavigationSignals.size(); ++signal)
			{
				const auto kind = signal == 0 ?
					RCBotNavigationExperience::Signal::STUCK_HEURISTIC :
					signal == 1 ?
						RCBotNavigationExperience::Signal::CAPTURE_ROUTE_EXHAUSTED :
						RCBotNavigationExperience::Signal::SQUAD_RECOVERY_FAILED;
				for (std::uint64_t count = 0;
					count < record.pendingNavigationSignals[signal]; ++count)
					m_navigationExperience.correlate(key, kind);
			}
			record.pendingNavigationSignals.fill(0);
			record.activeTransitionStarted = false;
			record.activeTransitionAttemptId = 0;
			record.activeTransitionFrom = -1;
			record.activeTransitionTo = -1;
			const std::string detail = experienceKeyDetail(key,
				observation.attemptId, RCBotNavigationExperience::ResultName(
					observation.result)) + ";duration_seconds=" +
				std::to_string(observation.traversalSeconds) +
				";progress_observations=" +
				std::to_string(attempt.GetProgressCount()) + ";reason=" +
				(reason != nullptr ? reason : "unspecified");
			writeEvent("NAV_EXPERIENCE_OBSERVATION", state, nullptr,
				detail.c_str(), intent.goalId, nullptr, nullptr, 0,
				&intent.recoveryEpisode, &intent, result);
		}

		void recordNavigationTransition(CBot* bot,
			const RCBotNavigation::NavigationIntent& intent,
			const RCBotNavigation::NavigationAttempt& attempt,
			const std::int64_t fromNode, const std::int64_t toNode,
			const RCBotNavigationExperience::Traversal traversal,
			const RCBotNavigationExperience::TransitionResult result,
			const RCBotNavigationExperience::CancellationKind cancellation,
			const char* reason, const BotState& state)
		{
			using namespace RCBotNavigationExperience;
			NavigationTransitionObservation observation;
			observation.mapIdentity = m_map;
			observation.provider = experienceProvider(intent.provider);
			observation.intentId = intent.id;
			observation.attemptId = attempt.GetAttemptId();
			observation.fromNode = fromNode;
			observation.toNode = toNode;
			observation.traversal = traversal;
			observation.result = result;
			observation.cancellation = cancellation;
			observation.botId = state.id;
			observation.attemptGeneration = attempt.GetProgressCount();
			observation.timestampMs = sessionMilliseconds();
			observation.durationSeconds = attempt.GetEndTime() > 0.0f ?
				(std::max)(0.0, static_cast<double>(attempt.GetEndTime()) -
					static_cast<double>(attempt.GetStartTime())) : 0.0;
			observation.progressUnits = attempt.GetProgressCount();
			const bool providerHasStableNodeIds =
				observation.provider == Provider::WAYPOINT ||
				observation.provider == Provider::NAVMESH;
			observation.transitionKnown = providerHasStableNodeIds &&
				fromNode >= 0 && toNode >= 0 && fromNode != toNode;
			if (!observation.transitionKnown)
			{
				observation.fromNode = -1;
				observation.toNode = -1;
				observation.traversal = Traversal::UNKNOWN;
				if (result != TransitionResult::CANCELLED)
					observation.result = TransitionResult::UNKNOWN;
			}
			if (!m_navigationExperience.recordTransition(observation))
				return;
			if (observation.transitionKnown)
			{
				auto record = m_bots.find(state.id);
				if (record != m_bots.end() &&
					record->second.activeTransitionAttemptId ==
						observation.attemptId &&
					record->second.activeTransitionFrom == observation.fromNode &&
					record->second.activeTransitionTo == observation.toNode)
					record->second.activeTransitionStarted = false;
			}

			const std::string transitionState = observation.transitionKnown ?
				"KNOWN_TRANSITION" : "UNKNOWN_TRANSITION";
			const std::string detail = std::string("provider=") +
				ProviderName(observation.provider) + ";intent_id=" +
				std::to_string(observation.intentId) + ";attempt_id=" +
				std::to_string(observation.attemptId) + ";attempt_generation=" +
				std::to_string(observation.attemptGeneration) +
				";path_request_id=0;from_node=" +
				std::to_string(observation.fromNode) + ";to_node=" +
				std::to_string(observation.toNode) + ";traversal_kind=" +
				TraversalName(observation.traversal) + ";transition_state=" +
				transitionState + ";result=" +
				TransitionResultName(observation.result) +
				";cancellation_kind=" +
				CancellationKindName(observation.cancellation) +
				";duration_seconds=" + std::to_string(observation.durationSeconds) +
				";progress_observations=" +
				std::to_string(observation.progressUnits) + ";reason=" +
				(reason != nullptr ? reason : "unspecified");
			writeEvent("NAV_TRANSITION_OBSERVATION", state, nullptr,
				detail.c_str(), -1, nullptr, nullptr, 0,
				&intent.recoveryEpisode, &intent, RCBotNavigation::Result::NONE);
		}

		void recordNavigationTransition(CBot* bot,
			const RCBotNavigation::NavigationIntent& intent,
			const RCBotNavigation::NavigationAttempt& attempt,
			const std::int64_t fromNode, const std::int64_t toNode,
			const RCBotNavigationExperience::Traversal traversal,
			const RCBotNavigationExperience::TransitionResult result,
			const RCBotNavigationExperience::CancellationKind cancellation,
			const char* reason)
		{
			if (!m_active || !validBot(bot))
				return;
			BotState state;
			if (!readState(bot, state))
				return;
			recordNavigationTransition(bot, intent, attempt, fromNode, toNode,
				traversal, result, cancellation, reason, state);
		}

		void markNavigationTransitionStarted(CBot* bot,
			const RCBotNavigation::NavigationIntent& intent,
			const RCBotNavigation::NavigationAttempt& attempt,
			const std::int64_t fromNode, const std::int64_t toNode,
			const RCBotNavigationExperience::Traversal traversal)
		{
			if (!m_active || !validBot(bot) || !attempt.IsActive() ||
				attempt.GetAttemptId() == 0 || intent.id == 0 || fromNode < 0 ||
				toNode < 0 || fromNode == toNode ||
				traversal == RCBotNavigationExperience::Traversal::NONE ||
				(intent.provider != RCBotNavigation::Provider::WAYPOINT &&
				 intent.provider != RCBotNavigation::Provider::NAVMESH))
				return;
			BotState state;
			if (!readState(bot, state))
				return;
			BotRecord& record = m_bots[state.id];
			if (record.activeTransitionAttemptId != attempt.GetAttemptId())
				return;
			record.activeTransitionFrom = fromNode;
			record.activeTransitionTo = toNode;
			record.activeTransitionTraversal = traversal;
			record.activeTransitionStarted = true;
		}

		void correlateNavigationSignal(CBot* bot,
			const RCBotNavigationExperience::Signal signal)
		{
			if (!m_active || !validBot(bot))
				return;
			BotState state;
			if (!readState(bot, state))
				return;
			auto it = m_bots.find(state.id);
			if (it == m_bots.end() || !it->second.hasLastNavigationKey)
				return;
			const std::uint64_t now = sessionMilliseconds();
			if (now < it->second.lastNavigationAtMs ||
				now - it->second.lastNavigationAtMs > 120000)
				return;
			bool pending = false;
			bool recorded = false;
			if (it->second.activeNavigationAttemptId != 0)
			{
				const std::size_t index = signal ==
					RCBotNavigationExperience::Signal::STUCK_HEURISTIC ? 0 :
					signal == RCBotNavigationExperience::Signal::CAPTURE_ROUTE_EXHAUSTED ?
						1 : 2;
				if (it->second.pendingNavigationSignals[index] <
					(std::numeric_limits<std::uint64_t>::max)())
					++it->second.pendingNavigationSignals[index];
				pending = true;
			}
			else
				recorded = m_navigationExperience.correlate(
					it->second.lastNavigationKey, signal);
			if (!recorded && !pending)
				return;
			const std::string detail = std::string("signal=") +
				RCBotNavigationExperience::SignalName(signal) +
				";correlation_window_ms=120000;pending_for_attempt=" +
				std::string(pending ? "1;" : "0;") +
				experienceKeyDetail(it->second.lastNavigationKey, 0, "CORRELATED");
			writeEvent("NAVIGATION_CORRELATED_SIGNAL", state, nullptr,
				detail.c_str(), -1, nullptr);
		}

		RCBotNavigationExperience::LearnedNavigationCost evaluateLearnedCost(
			const RCBotNavigationExperience::Provider provider,
			const std::int64_t fromNode, const std::int64_t toNode,
			const RCBotNavigationExperience::Traversal traversal) const
		{
			using namespace RCBotNavigationExperience;
			LearnedNavigationCost result;
			if (!m_active || provider == Provider::LOCAL_EXPLORER ||
				provider == Provider::UNKNOWN || fromNode < 0 || toNode < 0 ||
				fromNode == toNode || traversal == Traversal::NONE)
				return result;
			TransitionKey key{m_map, provider, fromNode, toNode, traversal};
			LearnedNavigationCostPolicy policy;
			policy.enabled = eta_nav_learning.GetBool();
			return m_navigationExperience.learnedCost(key, policy);
		}

		void recordLearnedEdgeConsidered(CBot* bot,
			const RCBotNavigationExperience::Provider provider,
			const std::int64_t fromNode, const std::int64_t toNode,
			const RCBotNavigationExperience::Traversal traversal,
			const double baseCost, const double effectiveCost,
			const RCBotNavigationExperience::LearnedNavigationCost& cost)
		{
			using namespace RCBotNavigationExperience;
			if (!m_active || bot == nullptr || !validBot(bot) ||
				!eta_nav_learning.GetBool() || cost.penalty <= 0.0 ||
				provider == Provider::LOCAL_EXPLORER || provider == Provider::UNKNOWN ||
				fromNode < 0 || toNode < 0 || fromNode == toNode ||
				traversal == Traversal::NONE)
				return;
			const TransitionKey key{m_map, provider, fromNode, toNode, traversal};
			if (m_loggedLearnedEdges.find(key) != m_loggedLearnedEdges.end())
				return;
			if (m_loggedLearnedEdges.size() >= 512)
				m_loggedLearnedEdges.erase(m_loggedLearnedEdges.begin());
			m_loggedLearnedEdges.insert(key);
			++m_learnedEdgesWithPenalty;
			m_maxObservedPenalty = (std::max)(m_maxObservedPenalty, cost.penalty);
			m_totalObservedPenalty += cost.penalty;
			++m_nonzeroPenaltyObservations;
			BotState state;
			if (!readState(bot, state))
				return;
			const std::string detail = std::string("provider=") +
				ProviderName(provider) + ";from_node=" + std::to_string(fromNode) +
				";to_node=" + std::to_string(toNode) + ";traversal_kind=" +
				TraversalName(traversal) + ";attempts=" +
				std::to_string(cost.attempts) + ";classified_attempts=" +
				std::to_string(cost.classifiedAttempts) + ";successes=" +
				std::to_string(cost.successes) + ";no_progress=" +
				std::to_string(cost.noProgress) + ";structural_failures=" +
				std::to_string(cost.structuralFailures) + ";distinct_failure_bots=" +
				std::to_string(cost.distinctFailureBots) + ";confidence=" +
				number(cost.confidence) + ";learned_penalty=" +
				number(cost.penalty) + ";base_cost=" + number(baseCost) +
				";effective_cost=" + number(effectiveCost);
			writeEvent("NAV_LEARNED_EDGE_CONSIDERED", state, nullptr,
				detail.c_str(), -1, nullptr);
		}

		void recordLearnedBehaviorChange(CBot* bot,
			const RCBotNavigationExperience::Provider provider,
			const std::int64_t fromNode, const std::int64_t toNode,
			const RCBotNavigationExperience::Traversal traversal,
			const double baseCost, const double effectiveCost)
		{
			using namespace RCBotNavigationExperience;
			if (!m_active || bot == nullptr || !validBot(bot) ||
				!eta_nav_learning.GetBool() || provider == Provider::LOCAL_EXPLORER ||
				provider == Provider::UNKNOWN || fromNode < 0 || toNode < 0 ||
				fromNode == toNode || traversal == Traversal::NONE)
				return;
			++m_learnedNavigationBehaviorChanges;
			BotState state;
			if (!readState(bot, state))
				return;
			const std::string detail = std::string("provider=") +
				ProviderName(provider) + ";from_node=" + std::to_string(fromNode) +
				";to_node=" + std::to_string(toNode) + ";traversal_kind=" +
				TraversalName(traversal) + ";base_cost=" + number(baseCost) +
				";effective_cost=" + number(effectiveCost) +
				";reason=learned-edge-preference-reordered";
			writeEvent("NAV_LEARNED_BEHAVIOR_CHANGE", state, nullptr,
				detail.c_str(), -1, nullptr);
		}

		std::uint64_t learnedBehaviorChanges() const
		{
			return m_learnedNavigationBehaviorChanges;
		}

		std::uint64_t recoveryEpisodeBehaviorChanges() const
		{
			return m_recoveryEpisodeBehaviorChanges;
		}

		std::uint64_t localAvoidanceBehaviorChanges() const
		{
			return m_localAvoidanceBehaviorChanges;
		}

		void recordLocalYieldBehaviorChange(CBot* bot, const char* reason)
		{
			if (!m_active || !validBot(bot))
				return;
			BotState state;
			if (!readState(bot, state))
				return;
			++m_localAvoidanceBehaviorChanges;
			writeEvent("ALLY_YIELD_MOVEMENT_SUPPRESSED", state, nullptr,
				reason != nullptr ? reason : "intentional-wait", -1, nullptr);
		}

		double maximumObservedPenalty() const { return m_maxObservedPenalty; }
		double averageObservedPenalty() const
		{
			return m_nonzeroPenaltyObservations == 0 ? 0.0 :
				m_totalObservedPenalty /
				static_cast<double>(m_nonzeroPenaltyObservations);
		}
		std::uint64_t learnedEdgesWithPenalty() const
		{
			return m_learnedEdgesWithPenalty;
		}

		int recordCommanderEvent(CBot* bot, const char* event,
			const Vector& targetOrigin, const char* detail, const edict_t* target)
		{
			if (!m_active || !validBot(bot) || event == nullptr)
				return 0;
			BotState state;
			if (!readState(bot, state))
				return 0;
			BotRecord& record = m_bots[state.id];
			int orderId = record.commanderOrder;
			if (std::string(event) == "COMMANDER_ORDER_RECEIVED")
			{
				orderId = ++m_nextCommanderOrderId;
				record.commanderOrder = orderId;
			}
			writeEvent(event, state, target, detail, -1, nullptr, &targetOrigin,
				orderId);
			if (std::string(event) == "COMMANDER_ORDER_COMPLETED" ||
				std::string(event) == "COMMANDER_ORDER_CANCELLED")
				record.commanderOrder = 0;
			return orderId;
		}

		void recordCapturePointState(const int entityIndex, const int pointIndex,
			const char* state)
		{
			if (!m_active || entityIndex < 0 || state == nullptr)
				return;
			const std::string value(state);
			auto previous = m_capturePointStates.find(entityIndex);
			if (previous != m_capturePointStates.end() && previous->second == value)
				return;
			m_capturePointStates[entityIndex] = value;
			std::string line = "{\"event_id\":" + std::to_string(++m_nextEventId) +
				",\"session_time\":" + number(elapsed()) +
				",\"game_time\":" + gameTime() +
				",\"event\":\"CAPTURE_POINT_STATE\",\"bot_id\":null" +
				",\"entity_index\":" + std::to_string(entityIndex) +
				",\"point_index\":" + std::to_string(pointIndex) +
				",\"state\":" + jsonString(value.c_str()) + "}\n";
			append(m_eventBuffer, line, true);
			++m_eventCount;
			++m_eventCounts["CAPTURE_POINT_STATE"];
		}

		void observeCommanderOrder(
			const EmpiresAnalysis::CommanderOrderObservation& observation)
		{
			if (!m_active || observation.recipientUserId <= 0)
				return;
			auto prior = m_commanderOrderObservations.find(
				observation.recipientUserId);
			const bool wasActive = prior != m_commanderOrderObservations.end() &&
				prior->second.commandType > 0;
			const bool isActive = observation.commandType > 0;
			if (!wasActive && !isActive)
			{
				m_commanderOrderObservations[observation.recipientUserId] = observation;
				return;
			}
			const auto same = [](const EmpiresAnalysis::CommanderOrderObservation& a,
				const EmpiresAnalysis::CommanderOrderObservation& b)
			{
				return a.commandType == b.commandType &&
					a.commanderUserId == b.commanderUserId && a.team == b.team &&
					a.squad == b.squad && a.targetEntityIndex == b.targetEntityIndex &&
					a.targetEntityKnown == b.targetEntityKnown &&
					a.targetPositionKnown == b.targetPositionKnown &&
					(!a.targetPositionKnown || (a.targetX == b.targetX &&
						a.targetY == b.targetY && a.targetZ == b.targetZ)) &&
					a.cancelTimeKnown == b.cancelTimeKnown &&
					(!a.cancelTimeKnown || a.cancelTime == b.cancelTime);
			};
			if (wasActive && isActive && same(prior->second, observation))
				return;

			const char* event = !wasActive ? "HUMAN_COMMANDER_ORDER_CREATED" :
				(!isActive ? "HUMAN_COMMANDER_ORDER_REMOVED" :
					"HUMAN_COMMANDER_ORDER_UPDATED");
			const auto& value = isActive ? observation : prior->second;
			std::string line = "{\"event_id\":" + std::to_string(++m_nextEventId) +
				",\"session_time\":" + number(elapsed()) +
				",\"game_time\":" + gameTime() +
				",\"event\":" + jsonString(event) +
				",\"commander_userid\":" + std::to_string(value.commanderUserId) +
				",\"recipient_userid\":" + std::to_string(value.recipientUserId) +
				",\"team\":" + std::to_string(value.team) +
				",\"squad\":" + std::to_string(value.squad) +
				",\"command_type\":" + std::to_string(value.commandType) +
				",\"target_entity_index\":" +
				(value.targetEntityKnown ? std::to_string(value.targetEntityIndex) : "null") +
				",\"target_position\":" +
				(value.targetPositionKnown ? "[" + number(value.targetX) + "," +
					number(value.targetY) + "," + number(value.targetZ) + "]" : "null") +
				",\"cancel_time\":" +
				(value.cancelTimeKnown ? number(value.cancelTime) : "null") + "}\n";
			append(m_eventBuffer, line, true);
			++m_eventCount;
			++m_eventCounts[event];
			m_commanderOrderObservations[observation.recipientUserId] = observation;
		}

		void recordNativeCaptureEvent(const char* eventName, const bool team,
			const int location)
		{
			if (!m_active || eventName == nullptr ||
				(std::strcmp(eventName, "point_captured") != 0 &&
				 std::strcmp(eventName, "point_neutral") != 0))
				return;
			std::string line = "{\"event_id\":" + std::to_string(++m_nextEventId) +
				",\"session_time\":" + number(elapsed()) +
				",\"game_time\":" + gameTime() +
				",\"event\":" + jsonString(eventName) +
				",\"team\":" + std::string(team ? "true" : "false") +
				",\"location\":" + std::to_string(location) +
				",\"point_index\":" + std::to_string(location) + "}\n";
			append(m_eventBuffer, line, true);
			++m_eventCount;
			++m_eventCounts[eventName];
		}

		void recordOrderProtocolEvent(CBot* bot,
			const RCBotOrderTelemetry::TelemetryEvent& event)
		{
			if (!m_active || !validBot(bot) ||
				event.type == RCBotOrderTelemetry::EventType::NONE)
				return;
			BotState state;
			if (!readState(bot, state))
				return;

			const RCBotOrder::OrderTransition& transition = event.transition;
			const char* eventName =
				RCBotOrderTelemetry::EventTypeName(event.type);
			const std::string previousState = transition.previousHasOrder ?
				std::string("\"") +
				RCBotOrder::OrderStateName(transition.previousState) + "\"" :
				"null";
			const std::string newState = transition.newHasOrder ?
				std::string("\"") +
				RCBotOrder::OrderStateName(transition.newState) + "\"" :
				"null";
			const std::string previousOutcome = transition.previousHasOrder ?
				std::string("\"") +
				RCBotOrder::OrderOutcomeName(transition.previousOutcome) + "\"" :
				"null";
			const std::string newOutcome = transition.newHasOrder ?
				std::string("\"") +
				RCBotOrder::OrderOutcomeName(transition.newOutcome) + "\"" :
				"null";
			const std::string outcome = transition.newHasOrder ? newOutcome :
				previousOutcome;
			const Vector target(transition.target.x, transition.target.y,
				transition.target.z);

			std::string line = "{\"event_id\":" +
				std::to_string(++m_nextEventId) +
				",\"session_time\":" + number(elapsed()) +
				",\"game_time\":" + gameTime() +
				",\"event\":" + jsonString(eventName) +
				",\"bot_id\":" + std::to_string(state.id) +
				",\"entity_index\":" + std::to_string(state.entityIndex) +
				",\"name\":" + jsonString(state.name.c_str()) +
				",\"team\":" + std::to_string(state.team) +
				",\"team_name\":" + jsonString(teamName(state.team)) +
				",\"class\":" + jsonString(className(state.classId)) +
				",\"position\":" + vectorJson(state.origin) +
				",\"goal\":" +
				(state.hasGoal ? vectorJson(state.goal) : "null") +
				",\"task\":" + jsonString(state.task.c_str()) +
				",\"schedule\":" + jsonString(state.schedule.c_str()) +
				",\"waypoint\":" + std::to_string(state.waypoint) +
				",\"enemy_index\":" + std::to_string(state.enemyIndex) +
				",\"enemy_userid\":" + std::to_string(state.enemyUserId) +
				",\"role_state\":" + jsonString(state.roleState.c_str()) +
				",\"data\":{\"order_id\":" +
				std::to_string(transition.orderId) +
				",\"generation\":" + std::to_string(transition.generation) +
				",\"issuer_id\":" + std::to_string(transition.issuerId) +
				",\"recipient_kind\":" + jsonString(
					RCBotOrder::RecipientKindName(transition.recipient.kind)) +
				",\"recipient_id\":" + std::to_string(transition.recipient.id) +
				",\"recipient_generation\":" +
				std::to_string(transition.recipient.generation) +
				",\"order_type\":" +
				jsonString(RCBotOrder::OrderTypeName(transition.orderType)) +
				",\"target_position\":" + vectorJson(target) +
				",\"operation\":" +
				jsonString(RCBotOrder::OrderOperationName(transition.operation)) +
				",\"previous_state\":" + previousState +
				",\"new_state\":" + newState +
				",\"previous_outcome\":" + previousOutcome +
				",\"new_outcome\":" + newOutcome +
				",\"outcome\":" + outcome +
				",\"order_outcome\":" + outcome +
				",\"detail_reason\":" +
				jsonString(RCBotOrder::DetailReasonName(transition.detailReason)) +
				",\"update_status\":" +
				jsonString(RCBotOrder::UpdateStatusName(transition.updateResult)) +
				",\"mutation_applied\":" +
				std::string(transition.mutationApplied ? "true" : "false") +
				",\"timestamp\":" + number(transition.timestamp);
			if (event.context.hasNavigationIntentId)
				line += ",\"navigation_intent_id\":" +
					std::to_string(event.context.navigationIntentId);
			if (event.context.hasNavigationAttemptId)
				line += ",\"navigation_attempt_id\":" +
					std::to_string(event.context.navigationAttemptId);
			if (event.context.hasProvider)
				line += ",\"provider\":" +
					jsonString(RCBotOrderTelemetry::NavigationProviderName(
						event.context.provider));
			if (event.context.hasNavigationTerminalResult)
				line += ",\"navigation_terminal_result\":" +
					jsonString(
						RCBotOrderTelemetry::NavigationTerminalResultName(
							event.context.navigationTerminalResult));
			else if (event.type == RCBotOrderTelemetry::EventType::ORDER_PROTOCOL_TERMINAL)
				line += ",\"navigation_terminal_result\":\"NONE\"";
			if (transition.recipient.kind == RCBotOrder::RecipientKind::SQUAD)
			{
				line += ",\"squad_id\":" +
					std::to_string(transition.recipient.id) +
					",\"squad_generation\":" +
					std::to_string(transition.recipient.generation);
			}
			if (event.correlation.hasParentOrder)
			{
				line += ",\"parent_order_id\":" +
					std::to_string(event.correlation.parentOrder.orderId) +
					",\"parent_generation\":" +
					std::to_string(event.correlation.parentOrder.generation);
			}
			if (event.correlation.hasMemberId)
				line += ",\"member_id\":" +
					std::to_string(event.correlation.memberId);
			if (event.correlation.hasChildExecution)
			{
				line += ",\"child_execution_id\":" +
					std::to_string(event.correlation.childExecution.orderId) +
					",\"child_execution_generation\":" +
					std::to_string(event.correlation.childExecution.generation);
			}
			if (event.correlation.hasAggregateOutcome)
				line += ",\"parent_aggregate_result\":" + jsonString(
					RCBotOrder::OrderOutcomeName(
						event.correlation.aggregateOutcome));
			line += "}}\n";
			append(m_eventBuffer, line, true);
			++m_eventCount;
			++m_eventCounts[eventName];
		}

		void recordSquadShadowEvent(CBot* bot,
			const RCBotEmpiresSquadRuntime::Telemetry& event)
		{
			if (!m_active || !validBot(bot))
				return;
			BotState state;
			if (!readState(bot, state))
				return;
			const char* eventName =
				RCBotEmpiresSquadRuntime::EventTypeName(event.event);
			std::string line = "{\"event_id\":" +
				std::to_string(++m_nextEventId) +
				",\"session_time\":" + number(elapsed()) +
				",\"game_time\":" + gameTime() +
				",\"event\":" + jsonString(eventName) +
				",\"bot_id\":" + std::to_string(state.id) +
				",\"entity_index\":" + std::to_string(state.entityIndex) +
				",\"name\":" + jsonString(state.name.c_str()) +
				",\"team\":" + std::to_string(state.team) +
				",\"team_name\":" + jsonString(teamName(state.team)) +
				",\"position\":" + vectorJson(state.origin) +
				",\"data\":{\"squad_id\":" +
				std::to_string(event.squadId) +
				",\"squad_generation\":" +
				std::to_string(event.squadGeneration) +
				",\"decision_revision\":" +
				std::to_string(event.decisionRevision) +
				",\"snapshot_fingerprint\":" +
				std::to_string(event.snapshotFingerprint) +
				",\"parent_order_id\":" +
				std::to_string(event.parentOrder.orderId) +
				",\"parent_generation\":" +
				std::to_string(event.parentOrder.generation) +
				",\"formation_generation\":" +
				std::to_string(event.formationGeneration) +
				",\"formation_type\":" + jsonString(
					RCBotEmpiresSquadRuntime::FormationTypeName(
						event.formationType)) +
				",\"member_id\":" + std::to_string(event.memberId) +
				",\"participation\":" + jsonString(
					RCBotSquadDecision::ParticipationStateName(
						event.participation)) +
				",\"slot_id\":" + std::to_string(event.slotId) +
				",\"claim_active\":" +
				std::string(event.claimActive ? "true" : "false") +
				",\"autonomous\":" +
				std::string(event.autonomous ? "true" : "false") +
				",\"movement_requested\":false}}\n";
			append(m_eventBuffer, line, true);
			++m_eventCount;
			++m_eventCounts[eventName];
		}

		void observeBot(CBot* bot)
		{
			if (!validBot(bot))
				return;
			BotState state;
			if (!readState(bot, state))
				return;
			const double now = m_active ? elapsed() :
				(gpGlobals != nullptr ? gpGlobals->curtime : 0.0);
			// Local Yielding is runtime behavior, while the analysis recorder is
			// optional. Keep the same passive detector alive for both modes.
			if (!m_active)
			{
				scanLocalConflicts(now);
				return;
			}
			BotRecord& record = m_bots[state.id];
			if (record.lastPoll >= 0.0 && now - record.lastPoll < kStatePollSeconds)
				return;
			record.lastPoll = now;

			if (!record.counted)
			{
				record.counted = true;
				++m_botCount;
				++m_teamCounts[std::to_string(state.team)];
				++m_classCounts[className(state.classId)];
				++m_roleCounts[roleName(state.classId)];
			}

			if (!record.observed)
			{
				writeEvent("BOT_JOIN", state, nullptr, "first-observed", -1, nullptr);
				if (state.alive)
					writeEvent("BOT_SPAWN", state, nullptr, "first-observed-alive", -1, nullptr);
				if (!state.taskKey.empty())
					writeEvent("TASK_STARTED", state, nullptr, "first-observed-task", -1, nullptr);
				if (state.hasEnemy)
					writeEvent("ENEMY_ACQUIRED", state, stateEnemy(state), "first-observed-enemy", -1, nullptr);
				record.observed = true;
				record.last = state;
				if (state.alive)
					++record.life;
				writeSnapshot(state, record, now);
				scanLocalConflicts(now);
				return;
			}

			const BotState old = record.last;
			if (old.userId != state.userId || old.entityIndex != state.entityIndex)
			{
				writeEvent("BOT_JOIN", state, nullptr, "identity-reused", -1, &old);
				record.stagnantSamples = 0;
				record.stuckReported = false;
				record.hasLastNavigationKey = false;
				record.activeNavigationAttemptId = 0;
				record.activeTransitionAttemptId = 0;
				record.activeTransitionFrom = -1;
				record.activeTransitionTo = -1;
				record.activeTransitionStarted = false;
				record.pendingNavigationSignals.fill(0);
			}
			else
			{
				observeLifecycle(state, old, record);
				observeGoalAndTask(state, old);
				observeNavigation(bot, state, old, record);
				observeCombat(state, old);
				observeSquad(state, old);
				observeRole(state, old);
			}

			record.last = state;
			writeSnapshot(state, record, now);
			scanLocalConflicts(now);
		}

		const char* sessionId() const { return m_sessionId.c_str(); }
		const char* sessionPath() const { return m_relativePath.c_str(); }
		const char* lastError() const { return m_lastError.c_str(); }
		int eventCount() const { return m_eventCount; }
		int botCount() const { return m_botCount; }
		double duration() const { return m_active ? elapsed() : m_duration; }

	private:
		struct LocalConflictBot
		{
			CBot* bot = nullptr;
			BotState state;
		};

		RCBotLocalAvoidance::LocalBotConflictObservation makeLocalConflictObservation(
			CBot* observerBot, const BotState& observer,
			CBot* otherBot, const BotState& other, const double now) const
		{
			using namespace RCBotLocalAvoidance;
			LocalBotConflictObservation observation;
			observation.observerBot = observer.id;
			observation.otherBot = other.id;
			observation.team = observer.team;
			observation.otherTeam = other.team;
			observation.timestamp = now;
			observation.observerValid = observer.valid;
			observation.otherValid = other.valid;
			observation.observerAlive = observer.alive;
			observation.otherAlive = other.alive;
			observation.observerPosition = observer.origin;
			observation.otherPosition = other.origin;
			observation.observerDesiredDirection = observer.desiredDirection;
			observation.otherDesiredDirection = other.desiredDirection;
			observation.observerVelocity = observer.velocity;
			observation.otherVelocity = other.velocity;
			observation.observerDesiredForward = observer.desiredForward;
			observation.observerDesiredSide = observer.desiredSide;
			observation.observerDesiredUp = observer.desiredUp;
			observation.observerWantsMovement = observer.wantsMovement;
			observation.otherWantsMovement = other.wantsMovement;
			observation.observerNavigationActive = observer.navigationActive;
			observation.otherNavigationActive = other.navigationActive;
			observation.observerIntentId = observer.navigationIntentId;
			observation.otherIntentId = other.navigationIntentId;
			observation.observerAttemptId = observer.navigationAttemptId;
			observation.otherAttemptId = other.navigationAttemptId;
			observation.observerProvider = observer.navigationProvider;
			observation.otherProvider = other.navigationProvider;
			observation.observerCurrentSegment = observer.currentSegment;
			observation.observerNextSegment = observer.nextSegment;
			observation.otherCurrentSegment = other.currentSegment;
			observation.otherNextSegment = other.nextSegment;
			observation.observerPrecisionContext = observer.precisionContext;
			observation.otherPrecisionContext = other.precisionContext;
			observation.observerPrecisionFlags = observer.precisionFlags;
			observation.otherPrecisionFlags = other.precisionFlags;
			observation.sameSquadKnown = observer.squadId >= 0 &&
				other.squadId >= 0;
			observation.sameSquad = observation.sameSquadKnown &&
				observer.squadId == other.squadId;
			observation.observerSquadId = observer.squadId;
			observation.otherSquadId = other.squadId;
			observation.observerFormationSlot = observer.formationSlot;
			observation.otherFormationSlot = other.formationSlot;
			observation.spatialMovementActive = observer.spatialMovementActive ||
				other.spatialMovementActive;
			observation.observerRemainingDistance = observer.destinationKnown ?
				(observer.origin - observer.destination).Length() : -1.0f;
			observation.otherRemainingDistance = other.destinationKnown ?
				(other.origin - other.destination).Length() : -1.0f;

			const Vector delta = other.origin - observer.origin;
			observation.distance = delta.Length();
			const float horizontalDistance = delta.Length2D();
			if (horizontalDistance <= 0.01f)
				return observation;

			const Vector corridorDirection = observer.desiredDirection;
			const float desiredLength = corridorDirection.Length2D();
			const float observerRadius = horizontalHullRadius(observerBot);
			const float otherRadius = horizontalHullRadius(otherBot);
			observation.corridorRadius = observerRadius + otherRadius +
				kLocalConflictHullPadding;
			if (desiredLength > 0.01f)
			{
				const Vector direction(corridorDirection.x / desiredLength,
					corridorDirection.y / desiredLength, 0.0f);
				const float forwardDistance = dot2d(delta, direction);
				observation.lateralCorridorDistance =
					std::fabs(cross2d(delta, direction));
				observation.otherIntersectsCorridor = forwardDistance >=
					-(observerRadius + otherRadius) &&
					forwardDistance <= kLocalConflictLookAhead + otherRadius &&
					observation.lateralCorridorDistance <= observation.corridorRadius;
				observation.allyThreat = observation.otherIntersectsCorridor;
				const float relativeClosing = dot2d(observer.velocity -
					other.velocity, direction);
				observation.closingSpeed = (std::max)(0.0f, relativeClosing);
				const float otherDesiredLength = other.desiredDirection.Length2D();
				if (otherDesiredLength > 0.01f)
				{
					const Vector otherDirection(other.desiredDirection.x /
						otherDesiredLength, other.desiredDirection.y /
						otherDesiredLength, 0.0f);
					observation.relativeHeading = dot2d(direction, otherDirection);
					observation.sameOrOpposedMotionKnown = true;
					observation.sameMotion = observation.relativeHeading >= 0.55f;
					observation.opposedMotion = observation.relativeHeading <= -0.55f;
				}
				observation.hullOverlap = horizontalDistance <=
					observerRadius + otherRadius;

				if (observation.otherIntersectsCorridor &&
					observerBot != nullptr && observerBot->getEdict() != nullptr &&
					observerBot->getEdict()->GetCollideable() != nullptr &&
					enginetrace != nullptr)
				{
					const Vector side(-direction.y, direction.x, 0.0f);
					const float sideDistance = observation.corridorRadius + 8.0f;
					const Vector leftEnd = observer.origin + side * sideDistance;
					const Vector rightEnd = observer.origin - side * sideDistance;
					observation.worldTraceKnown = true;
					observation.worldClearLeft = worldHullClear(observerBot,
						observer.origin, leftEnd);
					observation.worldClearRight = worldHullClear(observerBot,
						observer.origin, rightEnd);
					observation.worldBlocking = !observation.worldClearLeft ||
						!observation.worldClearRight;
				}
			}
			return observation;
		}

		BotState stateFromLocalConflict(
			const RCBotLocalAvoidance::LocalBotConflictObservation& observation) const
		{
			BotState state;
			state.valid = true;
			state.id = observation.observerBot;
			state.team = observation.team;
			state.alive = observation.observerAlive;
			state.origin = observation.observerPosition;
			state.destinationKnown = observation.observerWantsMovement;
			state.destination = observation.observerPosition +
				observation.observerDesiredDirection * 64.0f;
			state.navigationActive = observation.observerNavigationActive;
			state.navigationIntentId = observation.observerIntentId;
			state.navigationAttemptId = observation.observerAttemptId;
			state.navigationProvider = observation.observerProvider;
			state.currentSegment = observation.observerCurrentSegment;
			state.nextSegment = observation.observerNextSegment;
			state.precisionContext = observation.observerPrecisionContext;
			state.precisionFlags = observation.observerPrecisionFlags;
			state.inSquad = observation.sameSquadKnown && observation.sameSquad;
			state.formationSlot = observation.observerFormationSlot;
			return state;
		}

		void writeLocalConflictUpdate(const RCBotLocalAvoidance::EpisodeUpdate& update)
		{
			using namespace RCBotLocalAvoidance;
			if (update.lifecycle == Lifecycle::CONTINUE &&
				(update.episode.samples % 4u) != 0u)
				return;
			const LocalBotConflictObservation& observation =
				update.episode.lastObservation;
			BotState state = stateFromLocalConflict(observation);
			const std::string detail = std::string("lifecycle=") +
				LifecycleName(update.lifecycle) + ";conflict_type=" +
				ConflictTypeName(update.episode.type);
			const char* event = update.lifecycle == Lifecycle::BEGIN ?
				"ALLY_CONFLICT_BEGIN" : update.lifecycle == Lifecycle::CONTINUE ?
				"ALLY_CONFLICT_SAMPLE" : update.lifecycle == Lifecycle::RESOLVED ?
				"ALLY_CONFLICT_RESOLVED" : update.lifecycle ==
				Lifecycle::ESCALATED_TO_NO_PROGRESS ?
				"ALLY_CONFLICT_STUCK_CORRELATED" : "ALLY_CONFLICT_RESOLVED";
			writeEvent(event, state, nullptr, detail.c_str(), -1, nullptr,
				nullptr, 0, nullptr, nullptr, RCBotNavigation::Result::NONE,
				-1, &observation, &update.episode);
		}

		void writeLocalYieldEvaluation(
			const RCBotLocalYielding::Evaluation& evaluation,
			const RCBotLocalYielding::Event event)
		{
			if (!m_active || event == RCBotLocalYielding::Event::NONE)
				return;
			using namespace RCBotLocalAvoidance;
			const LocalBotConflictObservation& observation = evaluation.observation;
			BotState state = stateFromLocalConflict(observation);
			const RCBotLocalYielding::LocalYieldState& waiter = evaluation.wait;
			const std::string detail = std::string("canonical_pair=") +
				std::to_string(evaluation.lowBot) + "-" +
				std::to_string(evaluation.highBot) + ";conflict_episode_id=" +
				std::to_string(evaluation.episode.episodeId) +
				";conflict_type=" + ConflictTypeName(evaluation.episode.type) +
				";winner=" + std::to_string(evaluation.winnerBot) +
				";waiter=" + std::to_string(evaluation.waiterBot) +
				";role=" + RCBotLocalYielding::RoleName(waiter.role) +
				";reason=" + RCBotLocalYielding::ReasonName(waiter.reason) +
				";precision_context=observer:" +
				(observation.observerPrecisionContext ? "1" : "0") +
				",other:" + (observation.otherPrecisionContext ? "1" : "0") +
				";wait_age=" + number(waiter.waitAge) +
				";duration=" + number(evaluation.episode.duration) +
				";provider=" + observation.observerProvider + "/" +
				observation.otherProvider + ";intent=" +
				std::to_string(observation.observerIntentId) + "/" +
				std::to_string(observation.otherIntentId) + ";attempt=" +
				std::to_string(observation.observerAttemptId) + "/" +
				std::to_string(observation.otherAttemptId) +
				";priority_epoch=" + std::to_string(waiter.priorityEpoch);
			writeEvent(RCBotLocalYielding::EventName(event), state, nullptr,
				detail.c_str(), -1, nullptr, nullptr, 0, nullptr, nullptr,
				RCBotNavigation::Result::NONE, -1, &observation,
				&evaluation.episode);
		}

		void handleLocalConflictUpdate(
			const RCBotLocalAvoidance::EpisodeUpdate& update)
		{
			writeLocalConflictUpdate(update);
			if (!eta_local_yielding.GetBool())
				return;
			RCBotLocalYielding::Evaluation evaluation;
			if (update.lifecycle == RCBotLocalAvoidance::Lifecycle::RESOLVED ||
				update.lifecycle == RCBotLocalAvoidance::Lifecycle::ENDED_BY_LIFECYCLE)
				evaluation = RCBotLocalYielding::End(update.episode,
					update.episode.endTime,
					update.lifecycle == RCBotLocalAvoidance::Lifecycle::ENDED_BY_LIFECYCLE ?
						RCBotLocalYielding::Reason::OTHER :
						RCBotLocalYielding::Reason::WAIT_RESOLVED);
			else
				evaluation = RCBotLocalYielding::Observe(update.episode.lastObservation,
					update.episode, update.episode.lastSampleTime);
			if (evaluation.event == RCBotLocalYielding::Event::NONE)
				return;
			if (evaluation.event != RCBotLocalYielding::Event::DECISION &&
				evaluation.event != RCBotLocalYielding::Event::END)
				writeLocalYieldEvaluation(evaluation,
					RCBotLocalYielding::Event::DECISION);
			writeLocalYieldEvaluation(evaluation, evaluation.event);
		}

		void scanLocalConflicts(const double now)
		{
			if (m_localConflictLastScan >= 0.0 && now >= m_localConflictLastScan &&
				now - m_localConflictLastScan < kLocalConflictPollSeconds)
				return;
			if (now < m_localConflictLastScan)
				m_localConflictLastScan = -1.0;
			m_localConflictLastScan = now;
			++m_localConflictScanGeneration;
			if (!eta_local_yielding.GetBool() &&
				RCBotLocalYielding::StateCount() > 0)
				RCBotLocalYielding::Reset();
			std::vector<LocalConflictBot> bots;
			const int maxClients = (std::min)(CBotGlobals::maxClients(),
				static_cast<int>(RCBOT_MAXPLAYERS));
			for (int slot = 1; slot <= maxClients; ++slot)
			{
				CBot* candidate = CBots::getBot(slot);
				BotState state;
				if (candidate == nullptr || !readState(candidate, state))
					continue;
				bots.push_back({candidate, state});
			}
			for (std::size_t i = 0; i < bots.size(); ++i)
			{
				for (std::size_t j = i + 1; j < bots.size(); ++j)
				{
					if (bots[i].state.team != bots[j].state.team)
						continue;
					const auto first = makeLocalConflictObservation(
						bots[i].bot, bots[i].state, bots[j].bot, bots[j].state, now);
					const auto second = makeLocalConflictObservation(
						bots[j].bot, bots[j].state, bots[i].bot, bots[i].state, now);
					const auto observerProgressStalled = [this](const BotState& state)
					{
						if (!state.destinationKnown)
							return false;
						auto record = m_bots.find(state.id);
						if (record == m_bots.end() ||
							!record->second.hasLastLocalConflictProgress)
							return false;
						const float remaining = (state.origin - state.destination).Length();
						return record->second.lastLocalConflictRemainingDistance -
							remaining < kLocalConflictProgressEpsilon;
					};
					RCBotLocalAvoidance::LocalBotConflictObservation firstWithProgress = first;
					RCBotLocalAvoidance::LocalBotConflictObservation secondWithProgress = second;
					firstWithProgress.observerProgressStalled =
						observerProgressStalled(bots[i].state);
					secondWithProgress.observerProgressStalled =
						observerProgressStalled(bots[j].state);
					const bool firstCandidate =
						RCBotLocalAvoidance::Classify(firstWithProgress) !=
						RCBotLocalAvoidance::ConflictType::UNKNOWN;
					const bool secondCandidate =
						RCBotLocalAvoidance::Classify(secondWithProgress) !=
						RCBotLocalAvoidance::ConflictType::UNKNOWN;
					if (!firstCandidate && !secondCandidate)
						continue;
					const auto& selected = firstCandidate ? firstWithProgress :
						secondWithProgress;
					const auto updates = m_localConflictTracker.Observe(selected, now,
						m_localConflictScanGeneration);
					for (const auto& update : updates)
						handleLocalConflictUpdate(update);
				}
			}
			for (const LocalConflictBot& bot : bots)
			{
				auto record = m_bots.find(bot.state.id);
				if (record == m_bots.end() || !bot.state.destinationKnown)
					continue;
				record->second.hasLastLocalConflictProgress = true;
				record->second.lastLocalConflictRemainingDistance =
					(bot.state.origin - bot.state.destination).Length();
			}
			const auto resolved = m_localConflictTracker.EndUnseen(
				m_localConflictScanGeneration, now);
			for (const auto& update : resolved)
				handleLocalConflictUpdate(update);
		}

		void correlateLocalConflicts(const BotState& state,
			const std::uint64_t intentId, const std::uint64_t attemptId,
			const RCBotLocalAvoidance::CorrelationSignal signal)
		{
			if (intentId == 0 && attemptId == 0)
				return;
			const auto updates = m_localConflictTracker.Correlate(state.id,
				intentId, attemptId, elapsed(), signal);
			for (const auto& update : updates)
				writeLocalConflictUpdate(update);
		}

		std::uint64_t sessionMilliseconds() const
		{
			return static_cast<std::uint64_t>(std::max(0.0, elapsed()) * 1000.0);
		}

		static std::string navigationDataIdentity()
		{
			return "waypoint-version=" +
				std::to_string(CWaypoints::WAYPOINT_VERSION) +
				";waypoint-count=" + std::to_string(CWaypoints::numWaypoints()) +
				";navmesh-loaded=" +
				std::string(RCBotNavMesh::IsLoaded() ? "1" : "0") +
				";navmesh-area-count=" +
				std::to_string(RCBotNavMesh::GetAreaCount());
		}

		static RCBotNavigationExperience::Provider experienceProvider(
			const RCBotNavigation::Provider provider)
		{
			switch (provider)
			{
			case RCBotNavigation::Provider::WAYPOINT:
				return RCBotNavigationExperience::Provider::WAYPOINT;
			case RCBotNavigation::Provider::NAVMESH:
				return RCBotNavigationExperience::Provider::NAVMESH;
			case RCBotNavigation::Provider::LOCAL_EXPLORER:
				return RCBotNavigationExperience::Provider::LOCAL_EXPLORER;
			default:
				return RCBotNavigationExperience::Provider::UNKNOWN;
			}
		}

		static int nearestWaypoint(const Vector& position, const int team)
		{
			if (!position.IsValid())
				return -1;
			return CWaypointLocations::NearestWaypoint(position, 4096.0f, -1,
				false, false, false, nullptr, false, team);
		}

		static std::int64_t waypointNode(const Vector& position,
			const int team)
		{
			const int waypoint = nearestWaypoint(position, team);
			return waypoint >= 0 ? waypoint :
				RCBotNavigationExperience::QuantizedCell(position.x, position.y,
					position.z);
		}

		static bool validWaypoint(const int waypoint)
		{
			if (waypoint < 0 || waypoint >= CWaypoints::numWaypoints())
				return false;
			const CWaypoint* node = CWaypoints::getWaypoint(waypoint);
			return node != nullptr && node->isUsed();
		}

		static RCBotNavigationExperience::Traversal waypointTraversal(
			const RCBotNavigation::NavigationAttempt& attempt,
			const std::int64_t fallbackDestination)
		{
			const auto& segment = attempt.GetKey().segment;
			int destination = segment.nextWaypoint;
			if (!validWaypoint(destination))
				destination = fallbackDestination >= 0 &&
					fallbackDestination < CWaypoints::numWaypoints() ?
					static_cast<int>(fallbackDestination) : -1;
			CWaypoint* to = validWaypoint(destination) ?
				CWaypoints::getWaypoint(destination) : nullptr;
			if (to != nullptr && to->hasFlag(CWaypointTypes::W_FL_LADDER))
				return RCBotNavigationExperience::Traversal::LADDER;
			if (to != nullptr && to->hasFlag(CWaypointTypes::W_FL_JUMP))
				return RCBotNavigationExperience::Traversal::JUMP;
			if (validWaypoint(segment.currentWaypoint) && to != nullptr)
			{
				CWaypoint* from = CWaypoints::getWaypoint(
					segment.currentWaypoint);
				if (from != nullptr && from->getOrigin().z - to->getOrigin().z >= 48.0f)
					return RCBotNavigationExperience::Traversal::DROP;
			}
			return RCBotNavigationExperience::Traversal::NONE;
		}

		static RCBotNavigationExperience::Traversal waypointTransitionTraversal(
			const int fromWaypoint, const int toWaypoint)
		{
			using T = RCBotNavigationExperience::Traversal;
			if (!validWaypoint(fromWaypoint) || !validWaypoint(toWaypoint))
				return T::UNKNOWN;
			CWaypoint* from = CWaypoints::getWaypoint(fromWaypoint);
			CWaypoint* to = CWaypoints::getWaypoint(toWaypoint);
			if (from == nullptr || to == nullptr)
				return T::UNKNOWN;
			if (from->hasFlag(CWaypointTypes::W_FL_LADDER) ||
				to->hasFlag(CWaypointTypes::W_FL_LADDER))
				return T::LADDER;
			if (from->hasFlag(CWaypointTypes::W_FL_JUMP) ||
				to->hasFlag(CWaypointTypes::W_FL_JUMP))
				return T::JUMP;
			if (from->getOrigin().z - to->getOrigin().z >= 48.0f)
				return T::DROP;
			if (from->hasFlag(CWaypointTypes::W_FL_DOOR) ||
				to->hasFlag(CWaypointTypes::W_FL_DOOR) ||
				from->hasFlag(CWaypointTypes::W_FL_USE) ||
				to->hasFlag(CWaypointTypes::W_FL_USE) ||
				from->hasFlag(CWaypointTypes::W_FL_LIFT) ||
				to->hasFlag(CWaypointTypes::W_FL_LIFT) ||
				from->hasFlag(CWaypointTypes::W_FL_TELE_ENTRANCE) ||
				to->hasFlag(CWaypointTypes::W_FL_TELE_ENTRANCE) ||
				from->hasFlag(CWaypointTypes::W_FL_TELE_EXIT) ||
				to->hasFlag(CWaypointTypes::W_FL_TELE_EXIT) ||
				from->hasFlag(CWaypointTypes::W_FL_TELEPORT_CHEAT) ||
				to->hasFlag(CWaypointTypes::W_FL_TELEPORT_CHEAT))
				return T::OTHER;
			return T::NORMAL;
		}

		static RCBotNavigationExperience::Traversal transitionTraversalFor(
			const RCBotNavigation::Provider provider, const std::int64_t fromNode,
			const std::int64_t toNode)
		{
			if (provider == RCBotNavigation::Provider::WAYPOINT &&
				fromNode <= (std::numeric_limits<int>::max)() &&
				toNode <= (std::numeric_limits<int>::max)())
				return waypointTransitionTraversal(static_cast<int>(fromNode),
					static_cast<int>(toNode));
			return RCBotNavigationExperience::Traversal::UNKNOWN;
		}

		RCBotNavigationExperience::Key experienceKey(const BotState& state,
			const RCBotNavigation::NavigationIntent& intent,
			const RCBotNavigation::NavigationAttempt& attempt,
			const bool terminal) const
		{
			using namespace RCBotNavigationExperience;
			Key key;
			key.map = m_map;
			key.provider = experienceProvider(intent.provider);
			const Vector& start = attempt.GetStartPosition();
			const Vector& destination = intent.goalOrigin;
			switch (intent.provider)
			{
			case RCBotNavigation::Provider::WAYPOINT:
				key.startNode = waypointNode(start, state.team);
				if (intent.goalId >= 0 && validWaypoint(intent.goalId))
				{
					CWaypoint* goal = CWaypoints::getWaypoint(intent.goalId);
					key.destinationNode =
						(goal->getOrigin() - destination).LengthSqr() <= 64.0f * 64.0f ?
						intent.goalId : waypointNode(destination, state.team);
				}
				else
					key.destinationNode = waypointNode(destination, state.team);
				if (terminal)
					key.traversal = waypointTraversal(attempt,
						key.destinationNode);
				break;
			case RCBotNavigation::Provider::NAVMESH:
				key.startNode = RCBotNavMesh::GetNearestAreaId(start);
				key.destinationNode = RCBotNavMesh::GetNearestAreaId(destination);
				if (key.startNode < 0)
					key.startNode = QuantizedCell(start.x, start.y, start.z);
				if (key.destinationNode < 0)
					key.destinationNode = QuantizedCell(destination.x,
						destination.y, destination.z);
				break;
			case RCBotNavigation::Provider::LOCAL_EXPLORER:
				key.startNode = QuantizedCell(start.x, start.y, start.z);
				key.destinationNode = QuantizedCell(destination.x,
					destination.y, destination.z);
				if (std::fabs(destination.z - start.z) > 2.0f)
					key.traversal = destination.z > start.z ? Traversal::STEP_UP :
						Traversal::STEP_DOWN;
				break;
			default:
				break;
			}
			return key;
		}

		static RCBotNavigationExperience::Result experienceResult(
			const RCBotNavigation::Result result, const char* reason)
		{
			using R = RCBotNavigationExperience::Result;
			switch (result)
			{
			case RCBotNavigation::Result::COMPLETE: return R::SUCCESS;
			case RCBotNavigation::Result::PATH_FAILED: return R::PATH_FAILED;
			case RCBotNavigation::Result::PATH_STUCK: return R::PATH_STUCK;
			case RCBotNavigation::Result::UNREACHABLE: return R::UNREACHABLE;
			case RCBotNavigation::Result::CANCELLED:
			{
				const char* value = reason != nullptr ? reason : "";
				if (std::strstr(value, "dead") != nullptr ||
					std::strstr(value, "death") != nullptr ||
					std::strstr(value, "spawn") != nullptr ||
					std::strstr(value, "map-reset") != nullptr ||
					std::strstr(value, "team") != nullptr)
					return R::CANCELLED_LIFECYCLE;
				if (std::strstr(value, "replaced") != nullptr ||
					std::strstr(value, "preempt") != nullptr ||
					std::strstr(value, "goal-changed") != nullptr)
					return R::CANCELLED_REPLACEMENT;
				return R::CANCELLED_EXTERNAL;
			}
		default:
				return R::UNKNOWN;
			}
		}

		static RCBotNavigationExperience::TransitionResult transitionResultFor(
			const RCBotNavigation::Result result)
		{
			using T = RCBotNavigationExperience::TransitionResult;
			switch (result)
			{
			case RCBotNavigation::Result::COMPLETE:
				return T::TRANSITION_SUCCESS;
			case RCBotNavigation::Result::PATH_STUCK:
				return T::NO_PROGRESS;
			case RCBotNavigation::Result::PATH_FAILED:
			case RCBotNavigation::Result::UNREACHABLE:
				return T::STRUCTURAL_PATH_FAILURE;
			case RCBotNavigation::Result::CANCELLED:
				return T::CANCELLED;
			default:
				return T::UNKNOWN;
			}
		}

		static std::string experienceKeyDetail(
			const RCBotNavigationExperience::Key& key,
			const std::uint64_t attemptId, const char* result)
		{
			return std::string("provider=") +
				RCBotNavigationExperience::ProviderName(key.provider) +
				";start_node=" + std::to_string(key.startNode) +
				";destination_node=" + std::to_string(key.destinationNode) +
				";traversal=" +
				RCBotNavigationExperience::TraversalName(key.traversal) +
				";attempt_id=" + std::to_string(attemptId) +
				";result=" + (result != nullptr ? result : "UNKNOWN");
		}

		void resetSession()
		{
			m_active = false;
			m_events = FILESYSTEM_INVALID_HANDLE;
			m_snapshots = FILESYSTEM_INVALID_HANDLE;
			m_eventBuffer.clear();
			m_snapshotBuffer.clear();
			m_bots.clear();
			m_eventCounts.clear();
			m_capturePointStates.clear();
			m_commanderOrderObservations.clear();
			m_teamCounts.clear();
			m_classCounts.clear();
			m_roleCounts.clear();
			m_eventCount = 0;
			m_snapshotCount = 0;
			m_botCount = 0;
			m_userMarks = 0;
			m_nextEventId = 0;
			m_nextPathId = 0;
			m_nextCommanderOrderId = 0;
			m_duration = 0.0;
			m_navigationExperience.reset("unknown", "unknown");
			m_loggedLearnedEdges.clear();
			m_learnedNavigationBehaviorChanges = 0;
			m_learnedEdgesWithPenalty = 0;
			m_maxObservedPenalty = 0.0;
			m_totalObservedPenalty = 0.0;
			m_nonzeroPenaltyObservations = 0;
			m_localConflictTracker.Reset();
			m_localConflictLastScan = -1.0;
			m_localConflictScanGeneration = 0;
			m_localAvoidanceBehaviorChanges = 0;
		}

		std::string makeSessionId() const
		{
			const std::time_t now = std::time(nullptr);
			const std::tm* local = std::localtime(&now);
			char stamp[32] = {};
			if (local != nullptr)
				std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", local);
			if (stamp[0] == '\0')
				snprintf(stamp, sizeof(stamp), "unknown-%lld",
					static_cast<long long>(now));
			return stamp;
		}

		double elapsed() const
		{
			if (!m_active)
				return m_duration;
			return std::chrono::duration<double>(
				std::chrono::steady_clock::now() - m_start).count();
		}

		std::string gameTime() const
		{
			return gpGlobals != nullptr ? number(gpGlobals->curtime) : "null";
		}

		bool validBot(CBot* bot) const
		{
			return CBotGlobals::getCurrentMod() != nullptr &&
				CBotGlobals::getCurrentMod()->getBotType() == BOTTYPE_EMPIRES &&
				bot != nullptr && bot->inUse() &&
				!bot->isControlledTestOnly() && bot->getEdict() != nullptr &&
				!bot->getEdict()->IsFree();
		}

		bool readState(CBot* bot, BotState& state) const
		{
			if (!validBot(bot) || engine == nullptr || playerinfomanager == nullptr)
				return false;
			edict_t* edict = bot->getEdict();
			IPlayerInfo* info = playerinfomanager->GetPlayerInfo(edict);
			if (info == nullptr)
				return false;
			state.entityIndex = engine->IndexOfEdict(edict);
			state.userId = engine->GetPlayerUserId(edict);
			state.id = state.userId > 0 ? state.userId : 100000 + state.entityIndex;
			state.name = info->GetName() != nullptr ? info->GetName() : "<unknown>";
			state.team = info->GetTeamIndex();
			state.classId = bot->getDesiredClass();
			state.health = info->GetHealth();
			state.alive = info->IsConnected() && !info->IsObserver() &&
				!info->IsDead() && state.health > 0;
			state.origin = info->GetAbsOrigin();
			state.weapon = info->GetWeaponName() != nullptr ? info->GetWeaponName() : "";
			state.speed = bot->getSpeed();
			state.velocity = CBotGlobals::getVelocity(edict);
			state.desiredForward = bot->getForwardMoveOutput();
			state.desiredSide = bot->getSideMoveOutput();
			state.desiredUp = bot->getUpMoveOutput();
			state.attacking = bot->isHoldingPrimaryAttack();
			state.inSquad = bot->inSquad();
			state.squadLeader = state.inSquad && bot->isSquadLeader();
			if (state.inSquad && bot->getSquad() != nullptr)
			{
				CBotSquad* squad = bot->getSquad();
				state.formationSlot = squad->GetFormationPosition(edict);
				if (squad->GetLeader() != nullptr && engine != nullptr)
					state.squadId = engine->IndexOfEdict(squad->GetLeader());
			}
			state.hasGoal = bot->hasGoal();
			if (state.hasGoal && bot->getGoalOrigin() != nullptr)
				state.goal = *bot->getGoalOrigin();

			IBotNavigator* navigator = bot->getNavigator();
			if (navigator != nullptr)
			{
				state.goalWaypoint = navigator->getCurrentGoalID();
				state.routeSize = navigator->getRouteSize();
				if (navigator->hasNextPoint())
				{
					state.waypoint = navigator->getCurrentWaypointID();
					state.destination = navigator->getNextPoint();
					state.destinationKnown = state.destination.IsValid();
				}
			}
			if (bot->moveToIsValid())
			{
				state.destination = bot->getMoveTo();
				state.destinationKnown = state.destination.IsValid();
			}

			const RCBotNavigation::NavigationIntent& intent =
				bot->getNavigationIntent();
			const RCBotNavigation::NavigationAttempt& attempt =
				bot->getNavigationAttempt();
			state.navigationActive = bot->hasActiveNavigationIntent() ||
				bot->hasActiveNavigationAttempt();
			state.navigationIntentId = intent.id;
			state.navigationAttemptId = attempt.GetAttemptId();
			state.navigationProvider = navigationProviderName(intent.provider);
			if (state.navigationProvider == "UNKNOWN" && attempt.IsActive())
				state.navigationProvider = navigationProviderName(
					attempt.GetKey().provider);
			state.currentSegment = attempt.GetKey().segment.currentWaypoint;
			state.nextSegment = attempt.GetKey().segment.nextWaypoint;
			state.localYieldWaiting = bot->isLocalYieldWaiting();
			state.wantsMovement = state.alive && state.destinationKnown &&
				(state.destination - state.origin).Length2D() > 4.0f &&
				(state.navigationActive || state.routeSize > 0 || state.hasGoal ||
					!state.taskKey.empty() || bot->moveToIsValid());
			if (state.wantsMovement)
			{
				const Vector delta = state.destination - state.origin;
				const float length = delta.Length2D();
				if (length > 0.01f)
					state.desiredDirection = Vector(delta.x / length,
						delta.y / length, delta.z / length);
			}

			const auto addPrecisionFlags = [&](const int waypointId)
			{
				if (waypointId < 0 || waypointId >= CWaypoints::numWaypoints())
					return;
				CWaypoint* waypoint = CWaypoints::getWaypoint(waypointId);
				if (waypoint == nullptr || !waypoint->isUsed())
					return;
				const int flags = waypoint->getFlags();
				if (flags & CWaypointTypes::W_FL_LADDER)
					state.precisionFlags |= kPrecisionLadder;
				if (flags & CWaypointTypes::W_FL_JUMP)
					state.precisionFlags |= kPrecisionJump;
				if (flags & CWaypointTypes::W_FL_DOOR)
					state.precisionFlags |= kPrecisionDoor;
				if (flags & CWaypointTypes::W_FL_LIFT)
					state.precisionFlags |= kPrecisionLift;
				if (flags & (CWaypointTypes::W_FL_TELE_ENTRANCE |
					CWaypointTypes::W_FL_TELE_EXIT |
					CWaypointTypes::W_FL_TELEPORT_CHEAT))
					state.precisionFlags |= kPrecisionTeleport;
			};
			addPrecisionFlags(state.currentSegment);
			addPrecisionFlags(state.nextSegment);
			if (bot->onLadder() ||
				(navigator != nullptr && navigator->nextPointIsOnLadder()))
				state.precisionFlags |= kPrecisionLadder;
			if (navigator != nullptr &&
				(navigator->getCurrentFlags() & CWaypointTypes::W_FL_LIFT))
				state.precisionFlags |= kPrecisionLift;
			if (state.currentSegment >= 0 && state.nextSegment >= 0)
			{
				CWaypoint* from = CWaypoints::getWaypoint(state.currentSegment);
				CWaypoint* to = CWaypoints::getWaypoint(state.nextSegment);
				if (from != nullptr && to != nullptr &&
					from->getOrigin().z - to->getOrigin().z >= 48.0f)
					state.precisionFlags |= kPrecisionDrop;
			}
			if ((state.precisionFlags & (kPrecisionLadder | kPrecisionJump |
				kPrecisionDrop | kPrecisionDoor | kPrecisionLift |
				kPrecisionTeleport)) != 0 ||
				(state.wantsMovement && std::fabs(state.destination.z -
					state.origin.z) > 24.0f))
			{
				if (state.wantsMovement && std::fabs(state.destination.z -
					state.origin.z) > 24.0f)
					state.precisionFlags |= kPrecisionVertical;
				state.precisionContext = true;
			}

			edict_t* enemy = bot->getEnemy();
			if (enemy != nullptr && !enemy->IsFree())
			{
				state.hasEnemy = true;
				state.enemyIndex = engine->IndexOfEdict(enemy);
				state.enemyUserId = engine->GetPlayerUserId(enemy);
			}
			CBotSchedules* schedules = bot->getSchedule();
			if (schedules != nullptr && !schedules->isEmpty())
			{
				CBotSchedule* schedule = schedules->getCurrentSchedule();
				if (schedule != nullptr && schedule->getIDString() != nullptr)
					state.schedule = schedule->getIDString();
				CBotTask* task = schedules->getCurrentTask();
				if (task != nullptr)
				{
					char taskText[256] = {};
					task->debugString(taskText, sizeof(taskText));
					state.task = taskText;
					const std::size_t newline = state.task.find('\n');
					state.taskKey = state.task.substr(0, newline);
				}
			}
			state.roleState = roleState(state);
			state.recoveryEpisode = bot->getRecoveryEpisodeIdentity();
			state.valid = true;
			return true;
		}

		const char* roleName(const int classId) const
		{
			return className(classId);
		}

		std::string roleState(const BotState& state) const
		{
			const std::string combined = lowerCopy(state.schedule + " " + state.task +
				" " + state.weapon);
			if (state.classId == 3)
			{
				if (contains(combined, "resupply") || contains(combined, "ammo") ||
					contains(combined, "crate"))
					return "ENGINEER_RESUPPLY";
				if (contains(combined, "repair") || contains(combined, "support"))
					return "ENGINEER_REPAIR";
				if (contains(combined, "build") || contains(combined, "construct"))
					return "ENGINEER_BUILD";
			}
			if (state.classId == 2)
			{
				if (contains(combined, "mortar"))
					return "GRENADIER_MORTAR";
				if (contains(combined, "mine"))
					return "GRENADIER_MINE";
			}
			if (state.classId == 0 && contains(combined, "binocular"))
				return "SCOUT_RECON";
			return "";
		}

		const edict_t* stateEnemy(const BotState& state) const
		{
			if (engine == nullptr || state.enemyIndex < 0)
				return nullptr;
			return engine->PEntityOfEntIndex(state.enemyIndex);
		}

		void observeLifecycle(const BotState& state, const BotState& old,
			BotRecord& record)
		{
			if (!old.alive && state.alive)
			{
				++record.life;
				writeEvent("BOT_RESPAWN", state, nullptr, "alive-transition", -1, &old);
			}
			if (old.alive && !state.alive)
				writeEvent("BOT_DEATH", state, nullptr, "alive-transition", -1, &old);
		}

		void observeGoalAndTask(const BotState& state, const BotState& old)
		{
			const bool goalChanged = old.hasGoal != state.hasGoal ||
				old.goalWaypoint != state.goalWaypoint ||
				(old.hasGoal && state.hasGoal && !sameVector(old.goal, state.goal,
					kGoalChangeDistance));
			if (goalChanged)
				writeEvent("GOAL_CHANGED", state, nullptr, "state-transition", -1, &old);
			if (old.taskKey != state.taskKey || old.schedule != state.schedule)
			{
				if (!state.taskKey.empty())
					writeEvent("TASK_STARTED", state, nullptr, "state-transition", -1, &old);
			}
		}

		void observeNavigation(CBot* bot, const BotState& state, const BotState& old,
			BotRecord& record)
		{
			if (old.waypoint >= 0 && state.waypoint >= 0 &&
				old.waypoint != state.waypoint)
				writeEvent("WAYPOINT_REACHED", state, nullptr, "waypoint-transition", old.waypoint, &old);

			if (old.routeSize > 0 && state.routeSize <= 0 && state.destinationKnown)
			{
				const float distance = (state.origin - state.destination).Length();
				if (distance <= kDestinationReachDistance)
					writeEvent("DESTINATION_REACHED", state, nullptr, "route-ended-near-destination", -1, &old);
				else if (state.hasGoal || !state.taskKey.empty())
					writeEvent("FINAL_APPROACH_STARTED", state, nullptr, "route-ended-direct-target-remains", -1, &old);
			}

			const bool movingTowardTarget = state.alive && state.destinationKnown &&
				(state.routeSize > 0 || state.hasGoal || !state.taskKey.empty());
			if (state.localYieldWaiting || old.localYieldWaiting)
				record.stagnantSamples = 0;
			else if (movingTowardTarget && (state.origin - old.origin).Length() < kStuckMovementEpsilon)
				++record.stagnantSamples;
			else
				record.stagnantSamples = 0;
			if (record.stagnantSamples >= kStuckSamples && !record.stuckReported)
			{
				record.stuckReported = true;
				writeEvent("STUCK_DETECTED", state, nullptr, "sampled-no-progress;heuristic", state.waypoint, &old);
				correlateLocalConflicts(state, state.navigationIntentId,
					state.navigationAttemptId,
					RCBotLocalAvoidance::CorrelationSignal::STUCK_HEURISTIC);
				correlateNavigationSignal(bot,
					RCBotNavigationExperience::Signal::STUCK_HEURISTIC);
			}
			else if (record.stuckReported && record.stagnantSamples == 0)
			{
				record.stuckReported = false;
				writeEvent("RECOVERY", state, nullptr, "sampled-movement-resumed", state.waypoint, &old);
			}
		}

		void observeCombat(const BotState& state, const BotState& old)
		{
			if (!old.hasEnemy && state.hasEnemy)
			{
				const edict_t* enemy = stateEnemy(state);
				writeEvent("ENEMY_ACQUIRED", state, enemy, "visible-enemy-transition", -1, &old);
				writeEvent("COMBAT_STARTED", state, enemy, "enemy-acquired", -1, &old);
				if (state.classId == 0)
					writeEvent("SCOUT_TARGET_SELECTED", state, enemy, "enemy-acquired;existing-target-state", -1, &old);
			}
			else if (old.hasEnemy && !state.hasEnemy)
			{
				writeEvent("ENEMY_LOST", state, nullptr, "enemy-cleared-or-no-longer-visible", -1, &old);
				writeEvent("COMBAT_ENDED", state, nullptr, "enemy-lost", -1, &old);
			}
			if (old.weapon != state.weapon)
				writeEvent("WEAPON_CHANGED", state, nullptr, "playerinfo-weapon-transition", -1, &old);
			if (!old.attacking && state.attacking)
				writeEvent("ATTACK_STARTED", state, stateEnemy(state), "primary-attack-edge", -1, &old);
		}

		void observeSquad(const BotState& state, const BotState& old)
		{
			if (!old.inSquad && state.inSquad)
				writeEvent("SQUAD_JOIN", state, nullptr, "bot-squad-state-transition", -1, &old);
			else if (old.inSquad && !state.inSquad)
				writeEvent("SQUAD_LEAVE", state, nullptr, "bot-squad-state-transition", -1, &old);
			if (old.squadLeader != state.squadLeader)
				writeEvent("SQUAD_LEADER_CHANGED", state, nullptr, "bot-squad-leader-state-transition", -1, &old);
		}

		void observeRole(const BotState& state, const BotState& old)
		{
			if (state.roleState == old.roleState)
				return;
			if (state.roleState == "ENGINEER_RESUPPLY")
				writeEvent("ENGINEER_RESUPPLY_STARTED", state, nullptr, "role-state-observation", -1, &old);
			else if (state.roleState == "ENGINEER_REPAIR")
				writeEvent("ENGINEER_REPAIR_STARTED", state, nullptr, "role-state-observation", -1, &old);
			else if (state.roleState == "ENGINEER_BUILD")
				writeEvent("ENGINEER_BUILD_STARTED", state, nullptr, "role-state-observation", -1, &old);
			else if (state.roleState == "GRENADIER_MORTAR")
				writeEvent("GRENADIER_MORTAR", state, nullptr, "role-state-observation", -1, &old);
			else if (state.roleState == "GRENADIER_MINE")
				writeEvent("GRENADIER_MINE_PLACED", state, nullptr, "role-state-observation;placement-not-directly-observed", -1, &old);
			else if (old.roleState == "ENGINEER_RESUPPLY")
				writeEvent("ENGINEER_RESUPPLY_COMPLETED", state, nullptr, "role-state-ended;outcome-inferred", -1, &old);
			else if (old.roleState == "ENGINEER_REPAIR")
				writeEvent("ENGINEER_REPAIR_COMPLETED", state, nullptr, "role-state-ended;outcome-inferred", -1, &old);
			else if (old.roleState == "ENGINEER_BUILD")
				writeEvent("ENGINEER_BUILD_COMPLETED", state, nullptr, "role-state-ended;outcome-inferred", -1, &old);
		}

		void writeSnapshot(const BotState& state, BotRecord& record, const double now)
		{
			if (record.lastSnapshot >= 0.0 && now - record.lastSnapshot < kSnapshotSeconds)
				return;
			record.lastSnapshot = now;
			std::string line = "{\"session_time\":" + number(now) +
				",\"game_time\":" + gameTime() +
				",\"bot_id\":" + std::to_string(state.id) +
				",\"entity_index\":" + std::to_string(state.entityIndex) +
				",\"name\":" + jsonString(state.name.c_str()) +
				",\"team\":" + std::to_string(state.team) +
				",\"team_name\":" + jsonString(teamName(state.team)) +
				",\"class\":" + jsonString(className(state.classId)) +
				",\"health\":" + std::to_string(state.health) +
				",\"alive\":" + std::string(state.alive ? "true" : "false") +
				",\"position\":" + vectorJson(state.origin) +
				",\"goal\":" + (state.hasGoal ? vectorJson(state.goal) : "null") +
				",\"task\":" + jsonString(state.task.c_str()) +
				",\"schedule\":" + jsonString(state.schedule.c_str()) +
				",\"waypoint\":" + std::to_string(state.waypoint) +
				",\"goal_waypoint\":" + std::to_string(state.goalWaypoint) +
				",\"destination\":" + (state.destinationKnown ? vectorJson(state.destination) : "null") +
				",\"enemy_index\":" + std::to_string(state.enemyIndex) +
				",\"enemy_userid\":" + std::to_string(state.enemyUserId) +
				",\"squad\":" + std::string(state.inSquad ? "true" : "false") +
				",\"squad_leader\":" + std::string(state.squadLeader ? "true" : "false") +
				",\"local_yield_waiting\":" + std::string(
					state.localYieldWaiting ? "true" : "false") +
				",\"vehicle_entity\":null"
				",\"commander_order\":null"
				",\"stuck\":" + std::string(record.stuckReported ? "true" : "false") +
				",\"blocked\":null"
				",\"role_state\":" + jsonString(state.roleState.c_str()) +
				",\"life\":" + std::to_string(record.life) + "}\n";
			append(m_snapshotBuffer, line, false);
			++m_snapshotCount;
		}

		void writeEvent(const char* event, const BotState& state,
			const edict_t* target, const char* detail, const int waypointOverride,
			const BotState* old, const Vector* targetOrigin = nullptr,
			const int commanderOrder = 0,
			const RCBotRecovery::RecoveryEpisodeIdentity* episodeOverride = nullptr,
			const RCBotNavigation::NavigationIntent* navigationContext = nullptr,
			const RCBotNavigation::Result navigationResult =
				RCBotNavigation::Result::NONE,
			const int recoveryConsecutiveExhaustions = -1,
			const RCBotLocalAvoidance::LocalBotConflictObservation* localConflict = nullptr,
			const RCBotLocalAvoidance::LocalConflictEpisode* localConflictEpisode = nullptr)
		{
			if (!m_active || event == nullptr)
				return;
			const RCBotRecovery::RecoveryEpisodeIdentity& episode =
				episodeOverride != nullptr ? *episodeOverride : state.recoveryEpisode;
			std::string line = "{\"event_id\":" + std::to_string(++m_nextEventId) +
				",\"session_time\":" + number(elapsed()) +
				",\"game_time\":" + gameTime() +
				",\"event\":" + jsonString(event) +
				",\"bot_id\":" + std::to_string(state.id) +
				",\"entity_index\":" + std::to_string(state.entityIndex) +
				",\"name\":" + jsonString(state.name.c_str()) +
				",\"team\":" + std::to_string(state.team) +
				",\"team_name\":" + jsonString(teamName(state.team)) +
				",\"class\":" + jsonString(className(state.classId)) +
				",\"position\":" + vectorJson(state.origin) +
				",\"goal\":" + (state.hasGoal ? vectorJson(state.goal) : "null") +
				",\"task\":" + jsonString(state.task.c_str()) +
				",\"schedule\":" + jsonString(state.schedule.c_str()) +
				",\"waypoint\":" + std::to_string(waypointOverride >= 0 ? waypointOverride : state.waypoint) +
				",\"enemy_index\":" + std::to_string(state.enemyIndex) +
				",\"enemy_userid\":" + std::to_string(state.enemyUserId) +
				",\"role_state\":" + jsonString(state.roleState.c_str()) +
				",\"local_yield_waiting\":" + std::string(
					state.localYieldWaiting ? "true" : "false") +
				",\"life_generation\":" +
				std::to_string(episode.lifeGeneration) +
				",\"episode_id\":" + std::to_string(episode.episodeId) +
				",\"domain\":" + jsonString(
					RCBotRecovery::DomainName(episode.domain)) +
				",\"subject_id\":" + jsonString(episode.subjectId.c_str()) +
				",\"domain_generation\":" +
				std::to_string(episode.domainGeneration);
			if (navigationContext != nullptr)
			{
				line += ",\"intent_id\":" +
					std::to_string(navigationContext->id) +
					",\"attempt_id\":" +
					std::to_string(navigationContext->attemptId) +
					",\"provider\":" + jsonString(navigationProviderName(
						navigationContext->provider));
				if (navigationResult != RCBotNavigation::Result::NONE)
					line += ",\"terminal_result\":" +
						jsonString(navigationResultName(navigationResult));
			}
			if (std::strncmp(event, "RECOVERY_", 9) == 0 ||
				std::strcmp(event, "SQUAD_RECOVERY_WOULD_SUPPRESS") == 0)
				line += ",\"reason\":" + jsonString(
					detail != nullptr && *detail != '\0' ? detail : "UNKNOWN");
			if (recoveryConsecutiveExhaustions >= 0)
				line += ",\"consecutive_exhaustions\":" +
					std::to_string(recoveryConsecutiveExhaustions);
			line +=
				",\"data\":{";
			bool hasData = false;
			if (commanderOrder > 0)
			{
				line += "\"commander_order_id\":" +
					std::to_string(commanderOrder);
				hasData = true;
			}
			if (targetOrigin != nullptr)
			{
				if (hasData)
					line += ',';
				line += "\"target_position\":" + vectorJson(*targetOrigin);
				hasData = true;
			}
			if (detail != nullptr && *detail != '\0')
			{
				if (hasData)
					line += ',';
				line += "\"detail\":" + jsonString(detail);
				hasData = true;
			}
			if (target != nullptr && engine != nullptr && !target->IsFree())
			{
				if (hasData)
					line += ',';
				const int targetIndex = engine->IndexOfEdict(const_cast<edict_t*>(target));
				const bool targetIsClient = targetIndex >= 1 &&
					targetIndex <= gpGlobals->maxClients;
				IPlayerInfo* info = targetIsClient && playerinfomanager != nullptr ?
					playerinfomanager->GetPlayerInfo(const_cast<edict_t*>(target)) : nullptr;
				const int targetUserId = targetIsClient ?
					engine->GetPlayerUserId(const_cast<edict_t*>(target)) : -1;
				const int targetTeam = targetIsClient && info != nullptr ?
					info->GetTeamIndex() : -1;
				line += "\"target\":{";
				line += "\"entity_index\":" + std::to_string(targetIndex);
				line += ",\"userid\":" + std::to_string(targetUserId);
				line += ",\"team\":" + std::to_string(targetTeam);
				line += ",\"position\":" + vectorJson(CBotGlobals::entityOrigin(const_cast<edict_t*>(target))) + "}";
				hasData = true;
			}
			if (old != nullptr)
			{
				if (hasData)
					line += ',';
				line += "\"previous_task\":" + jsonString(old->taskKey.c_str()) +
					",\"previous_schedule\":" + jsonString(old->schedule.c_str()) +
					",\"previous_waypoint\":" + std::to_string(old->waypoint) +
					",\"previous_goal_waypoint\":" + std::to_string(old->goalWaypoint);
				hasData = true;
			}
			if (localConflict != nullptr)
			{
				if (hasData)
					line += ',';
				line += "\"local_conflict\":{";
				line += "\"observer_bot\":" +
					std::to_string(localConflict->observerBot) +
					",\"other_bot\":" + std::to_string(localConflict->otherBot) +
					",\"team\":" + std::to_string(localConflict->team) +
					",\"other_team\":" + std::to_string(localConflict->otherTeam) +
					",\"observer_position\":" + vectorJson(
						localConflict->observerPosition) +
					",\"other_position\":" + vectorJson(
						localConflict->otherPosition) +
					",\"observer_desired_direction\":" + vectorJson(
						localConflict->observerDesiredDirection) +
					",\"other_desired_direction\":" + vectorJson(
						localConflict->otherDesiredDirection) +
					",\"observer_velocity\":" + vectorJson(
						localConflict->observerVelocity) +
						",\"other_velocity\":" + vectorJson(
						localConflict->otherVelocity) +
						",\"observer_wants_movement\":" + std::string(
						localConflict->observerWantsMovement ? "true" : "false") +
					",\"other_wants_movement\":" + std::string(
						localConflict->otherWantsMovement ? "true" : "false") +
					",\"other_intersects_corridor\":" + std::string(
						localConflict->otherIntersectsCorridor ? "true" : "false") +
					",\"observer_progress_stalled\":" + std::string(
						localConflict->observerProgressStalled ? "true" : "false") +
					",\"observer_remaining_distance\":" + number(
						localConflict->observerRemainingDistance) +
					",\"other_remaining_distance\":" + number(
						localConflict->otherRemainingDistance) +
						",\"desired_forward\":" + number(
							localConflict->observerDesiredForward) +
						",\"desired_side\":" + number(
							localConflict->observerDesiredSide) +
						",\"desired_up\":" + number(
							localConflict->observerDesiredUp) +
					",\"distance\":" + number(localConflict->distance) +
					",\"closing_speed\":" + number(
						localConflict->closingSpeed) +
					",\"relative_heading\":" + number(
						localConflict->relativeHeading) +
					",\"lateral_corridor_distance\":" + number(
						localConflict->lateralCorridorDistance) +
					",\"corridor_radius\":" + number(
						localConflict->corridorRadius) +
					",\"hull_overlap\":" + std::string(
						localConflict->hullOverlap ? "true" : "false") +
					",\"observer_navigation_active\":" + std::string(
						localConflict->observerNavigationActive ? "true" : "false") +
					",\"other_navigation_active\":" + std::string(
						localConflict->otherNavigationActive ? "true" : "false") +
					",\"observer_intent_id\":" + std::to_string(
						localConflict->observerIntentId) +
					",\"other_intent_id\":" + std::to_string(
						localConflict->otherIntentId) +
					",\"observer_attempt_id\":" + std::to_string(
						localConflict->observerAttemptId) +
					",\"other_attempt_id\":" + std::to_string(
						localConflict->otherAttemptId) +
					",\"observer_provider\":" + jsonString(
						localConflict->observerProvider.c_str()) +
					",\"other_provider\":" + jsonString(
						localConflict->otherProvider.c_str()) +
					",\"observer_current_segment\":" + std::to_string(
						localConflict->observerCurrentSegment) +
					",\"observer_next_segment\":" + std::to_string(
						localConflict->observerNextSegment) +
					",\"other_current_segment\":" + std::to_string(
						localConflict->otherCurrentSegment) +
					",\"other_next_segment\":" + std::to_string(
						localConflict->otherNextSegment) +
					",\"observer_precision\":" + std::string(
						localConflict->observerPrecisionContext ? "true" : "false") +
					",\"other_precision\":" + std::string(
						localConflict->otherPrecisionContext ? "true" : "false") +
					",\"observer_precision_flags\":" + std::to_string(
						localConflict->observerPrecisionFlags) +
					",\"other_precision_flags\":" + std::to_string(
						localConflict->otherPrecisionFlags) +
					",\"same_squad_known\":" + std::string(
						localConflict->sameSquadKnown ? "true" : "false") +
					",\"same_squad\":" + std::string(
					localConflict->sameSquad ? "true" : "false") +
					",\"observer_squad_id\":" + std::to_string(
					localConflict->observerSquadId) +
					",\"other_squad_id\":" + std::to_string(
					localConflict->otherSquadId) +
					",\"observer_formation_slot\":" + std::to_string(
						localConflict->observerFormationSlot) +
					",\"other_formation_slot\":" + std::to_string(
						localConflict->otherFormationSlot) +
					",\"spatial_movement_active\":" + std::string(
						localConflict->spatialMovementActive ? "true" : "false") +
					",\"world_trace_known\":" + std::string(
						localConflict->worldTraceKnown ? "true" : "false") +
					",\"world_clear_left\":" + std::string(
						localConflict->worldClearLeft ? "true" : "false") +
					",\"world_clear_right\":" + std::string(
						localConflict->worldClearRight ? "true" : "false") +
					",\"world_blocking\":" + std::string(
						localConflict->worldBlocking ? "true" : "false") +
					",\"ally_threat\":" + std::string(
						localConflict->allyThreat ? "true" : "false");
				if (localConflictEpisode != nullptr)
				{
					line += ",\"conflict_episode_id\":" + std::to_string(
						localConflictEpisode->episodeId) +
						",\"conflict_type\":" + jsonString(
						RCBotLocalAvoidance::ConflictTypeName(localConflictEpisode->type)) +
						",\"episode_lifecycle\":" + jsonString(
						RCBotLocalAvoidance::LifecycleName(localConflictEpisode->lifecycle)) +
						",\"episode_start_time\":" + number(
						localConflictEpisode->startTime) +
						",\"episode_duration\":" + number(
						localConflictEpisode->duration) +
						",\"episode_min_distance\":" + number(
						localConflictEpisode->minDistance) +
						",\"episode_max_closing_speed\":" + number(
						localConflictEpisode->maxClosingSpeed) +
						",\"episode_samples\":" + std::to_string(
						localConflictEpisode->samples) +
						",\"observer_wanted_duration\":" + number(
						localConflictEpisode->observerWantedDuration) +
						",\"progress_before\":" + number(
						localConflictEpisode->progressBefore) +
						",\"progress_during\":" + number(
						localConflictEpisode->progressDuring) +
						",\"progress_after\":" + number(
						localConflictEpisode->progressAfter) +
						",\"stuck_correlated\":" + std::string(
						localConflictEpisode->stuckCorrelated ? "true" : "false");
					const std::uint8_t correlationMask =
						localConflictEpisode->correlationMask;
					line += ",\"correlation_mask\":" + std::to_string(
						correlationMask) +
						",\"nav_progress_correlated\":" + std::string(
						(correlationMask & (1u << static_cast<std::uint8_t>(
							RCBotLocalAvoidance::CorrelationSignal::NAV_PROGRESS))) != 0 ?
							"true" : "false") +
						",\"nav_no_progress_correlated\":" + std::string(
						(correlationMask & (1u << static_cast<std::uint8_t>(
							RCBotLocalAvoidance::CorrelationSignal::NAV_NO_PROGRESS))) != 0 ?
							"true" : "false") +
						",\"path_stuck_correlated\":" + std::string(
						(correlationMask & (1u << static_cast<std::uint8_t>(
							RCBotLocalAvoidance::CorrelationSignal::PATH_STUCK))) != 0 ?
							"true" : "false") +
						",\"heuristic_stuck_correlated\":" + std::string(
						(correlationMask & (1u << static_cast<std::uint8_t>(
							RCBotLocalAvoidance::CorrelationSignal::STUCK_HEURISTIC))) != 0 ?
							"true" : "false");
				}
				line += '}';
				hasData = true;
			}
			line += "}}\n";
			append(m_eventBuffer, line, true);
			++m_eventCount;
			++m_eventCounts[event];
		}

		void append(std::string& buffer, const std::string& value, const bool event)
		{
			buffer += value;
			if (buffer.size() >= kFlushBytes)
				flushBuffer(event);
		}

		void flushBuffer(const bool event)
		{
			std::string& buffer = event ? m_eventBuffer : m_snapshotBuffer;
			FileHandle_t handle = event ? m_events : m_snapshots;
			if (buffer.empty())
				return;
			if (handle == FILESYSTEM_INVALID_HANDLE || filesystem == nullptr)
			{
				disable("session file handle became invalid");
				return;
			}
			const int expected = static_cast<int>(buffer.size());
			const int written = filesystem->Write(buffer.data(), expected, handle);
			if (written != expected)
			{
				buffer.clear();
				disable("short write while flushing analysis JSONL");
				return;
			}
			buffer.clear();
		}

		bool flush()
		{
			if (!m_eventBuffer.empty())
				flushBuffer(true);
			if (!m_snapshotBuffer.empty())
				flushBuffer(false);
			return m_active && m_lastError.empty();
		}

		void closeFiles()
		{
			if (filesystem == nullptr)
				return;
			if (m_events != FILESYSTEM_INVALID_HANDLE)
			{
				filesystem->Flush(m_events);
				filesystem->Close(m_events);
				m_events = FILESYSTEM_INVALID_HANDLE;
			}
			if (m_snapshots != FILESYSTEM_INVALID_HANDLE)
			{
				filesystem->Flush(m_snapshots);
				filesystem->Close(m_snapshots);
				m_snapshots = FILESYSTEM_INVALID_HANDLE;
			}
		}

		bool writeWholeFile(const std::string& path, const std::string& contents)
		{
			if (filesystem == nullptr)
				return false;
			FileHandle_t handle = filesystem->Open(path.c_str(), "wb", kPathId);
			if (handle == FILESYSTEM_INVALID_HANDLE)
				return false;
			const int expected = static_cast<int>(contents.size());
			const int written = filesystem->Write(contents.data(), expected, handle);
			filesystem->Flush(handle);
			filesystem->Close(handle);
			return written == expected;
		}

		std::string makeSummary() const
		{
			std::string result = "{\n  \"session_id\": " + jsonString(m_sessionId.c_str()) +
				",\n  \"map\": " + jsonString(m_map.c_str()) +
				",\n  \"duration_seconds\": " + number(m_duration) +
				",\n  \"bot_count\": " + std::to_string(m_botCount) +
				",\n  \"event_count\": " + std::to_string(m_eventCount) +
				",\n  \"events_per_second\": " + number(m_duration > 0.0 ? m_eventCount / m_duration : 0.0) +
				",\n  \"snapshot_count\": " + std::to_string(m_snapshotCount) +
				",\n  \"snapshots_per_second\": " + number(m_duration > 0.0 ? m_snapshotCount / m_duration : 0.0) +
				",\n  \"spawns\": " + std::to_string(count("BOT_SPAWN")) +
				",\n  \"respawns\": " + std::to_string(count("BOT_RESPAWN")) +
				",\n  \"deaths\": " + std::to_string(count("BOT_DEATH")) +
				",\n  \"kills\": " + std::to_string(count("KILL")) +
				",\n  \"combat_engagements\": " + std::to_string(count("COMBAT_STARTED")) +
				",\n  \"objective_selected\": " + std::to_string(count("OBJECTIVE_SELECTED")) +
				",\n  \"capture_starts\": " + std::to_string(count("CAPTURE_STARTED")) +
				",\n  \"captures\": " + std::to_string(count("CAPTURE_COMPLETED")) +
				",\n  \"path_requests\": " + std::to_string(count("PATH_REQUEST")) +
				",\n  \"path_failures\": " + std::to_string(count("PATH_FAILURE")) +
				",\n  \"retries\": " + std::to_string(count("RETRY")) +
				",\n  \"stuck\": " + std::to_string(count("STUCK_DETECTED")) +
				",\n  \"navigation_experience_observations\": " +
				std::to_string(m_navigationExperience.totalObservations()) +
				",\n  \"navigation_successes\": " +
				std::to_string(m_navigationExperience.resultCount(
					RCBotNavigationExperience::Result::SUCCESS)) +
				",\n  \"navigation_true_failures\": " +
				std::to_string(
					m_navigationExperience.resultCount(RCBotNavigationExperience::Result::PATH_FAILED) +
					m_navigationExperience.resultCount(RCBotNavigationExperience::Result::PATH_STUCK) +
					m_navigationExperience.resultCount(RCBotNavigationExperience::Result::UNREACHABLE)) +
				",\n  \"navigation_stuck\": " +
				std::to_string(m_navigationExperience.resultCount(
					RCBotNavigationExperience::Result::PATH_STUCK)) +
				",\n  \"navigation_unreachable\": " +
				std::to_string(m_navigationExperience.resultCount(
					RCBotNavigationExperience::Result::UNREACHABLE)) +
				",\n  \"navigation_external_cancellations\": " +
				std::to_string(m_navigationExperience.resultCount(
					RCBotNavigationExperience::Result::CANCELLED_EXTERNAL)) +
				",\n  \"navigation_replacement_cancellations\": " +
				std::to_string(m_navigationExperience.resultCount(
					RCBotNavigationExperience::Result::CANCELLED_REPLACEMENT)) +
				",\n  \"navigation_lifecycle_cancellations\": " +
				std::to_string(m_navigationExperience.resultCount(
					RCBotNavigationExperience::Result::CANCELLED_LIFECYCLE)) +
				",\n  \"navigation_experience_keys\": " +
				std::to_string(m_navigationExperience.entries().size()) +
				",\n  \"navigation_experience_evictions\": " +
				std::to_string(m_navigationExperience.evictions()) +
				",\n  \"navigation_hotspots\": " +
				std::to_string(m_navigationExperience.hotspots().size()) +
				",\n  \"navigation_transition_observations\": " +
				std::to_string(m_navigationExperience.transitionObservationCount()) +
				",\n  \"navigation_transition_keys\": " +
				std::to_string(m_navigationExperience.uniqueTransitionKeys()) +
				",\n  \"navigation_transition_successes\": " +
				std::to_string(m_navigationExperience.transitionResultCount(
					RCBotNavigationExperience::TransitionResult::TRANSITION_SUCCESS)) +
				",\n  \"navigation_transition_no_progress\": " +
				std::to_string(m_navigationExperience.transitionResultCount(
					RCBotNavigationExperience::TransitionResult::NO_PROGRESS)) +
				",\n  \"navigation_transition_structural_failures\": " +
				std::to_string(m_navigationExperience.transitionResultCount(
					RCBotNavigationExperience::TransitionResult::STRUCTURAL_PATH_FAILURE)) +
				",\n  \"navigation_transition_cancellations\": " +
				std::to_string(m_navigationExperience.transitionResultCount(
					RCBotNavigationExperience::TransitionResult::CANCELLED)) +
				",\n  \"navigation_transition_unknown\": " +
				std::to_string(m_navigationExperience.unknownTransitionCount()) +
				",\n  \"navigation_transition_evictions\": " +
				std::to_string(m_navigationExperience.transitionEvictions()) +
				",\n  \"navigation_transition_hotspots\": " +
				std::to_string(m_navigationExperience.transitionHotspots().size()) +
				",\n  \"learned_navigation_behavior_changes\": " +
				std::to_string(m_learnedNavigationBehaviorChanges) +
				",\n  \"local_avoidance_behavior_changes\": " +
				std::to_string(m_localAvoidanceBehaviorChanges) +
				",\n  \"local_yielding_enabled\": " +
				std::string(eta_local_yielding.GetBool() ? "true" : "false") +
				",\n  \"local_yield_sticky_seconds\": " + number(
					RCBotLocalYielding::Policy().stickySeconds) +
				",\n  \"local_yield_max_wait_seconds\": " + number(
					RCBotLocalYielding::Policy().maxWaitSeconds) +
				",\n  \"local_avoidance_gate\": \"LOCAL_AVOIDANCE_SIGNAL_READY\"" +
				",\n  \"local_conflict_episode_records\": " +
				std::to_string(m_localConflictTracker.Size()) +
				",\n  \"local_conflict_active_episodes\": " +
				std::to_string(m_localConflictTracker.ActiveCount()) +
				",\n  \"local_conflict_evictions\": " +
				std::to_string(m_localConflictTracker.Evictions()) +
				",\n  \"local_conflict_begins\": " +
				std::to_string(count("ALLY_CONFLICT_BEGIN")) +
				",\n  \"local_conflict_samples\": " +
				std::to_string(count("ALLY_CONFLICT_SAMPLE")) +
				",\n  \"local_conflict_resolved\": " +
				std::to_string(count("ALLY_CONFLICT_RESOLVED")) +
				",\n  \"local_conflict_stuck_correlated\": " +
				std::to_string(count("ALLY_CONFLICT_STUCK_CORRELATED")) +
				",\n  \"local_yield_decisions\": " +
				std::to_string(count("ALLY_YIELD_DECISION")) +
				",\n  \"local_yield_begins\": " +
				std::to_string(count("ALLY_YIELD_BEGIN")) +
				",\n  \"local_yield_reevaluations\": " +
				std::to_string(count("ALLY_YIELD_REEVALUATE")) +
				",\n  \"local_yield_ends\": " +
				std::to_string(count("ALLY_YIELD_END")) +
				",\n  \"local_yield_starvation_rotations\": " +
				std::to_string(count("ALLY_YIELD_STARVATION_ROTATE")) +
				",\n  \"recovery_episode_behavior_changes\": " +
				std::to_string(m_recoveryEpisodeBehaviorChanges) +
				",\n  \"squad_recovery_shadow_suppressions\": " +
				std::to_string(count("SQUAD_RECOVERY_WOULD_SUPPRESS")) +
				",\n  \"learned_edges_with_penalty\": " +
				std::to_string(m_learnedEdgesWithPenalty) +
				",\n  \"maximum_observed_penalty\": " +
				number(m_maxObservedPenalty) +
				",\n  \"average_nonzero_penalty\": " +
				number(averageObservedPenalty()) +
				",\n  \"learned_costs_enabled\": " +
				std::string(eta_nav_learning.GetBool() ? "true" : "false") +
				",\n  \"blocked\": " + std::to_string(count("BLOCKED_DETECTED")) +
				",\n  \"recoveries\": " + std::to_string(count("RECOVERY")) +
				",\n  \"user_marks\": " + std::to_string(m_userMarks) +
				",\n  \"engineer_events\": " + std::to_string(countPrefix("ENGINEER_")) +
				",\n  \"grenadier_events\": " + std::to_string(countPrefix("GRENADIER_")) +
				",\n  \"scout_events\": " + std::to_string(countPrefix("SCOUT_")) +
				",\n  \"squad_events\": " + std::to_string(countPrefix("SQUAD_")) +
				",\n  \"commander_orders\": " + std::to_string(
					countPrefix("COMMANDER_ORDER_") +
					countPrefix("HUMAN_COMMANDER_ORDER_")) +
			",\n  \"teams\": " + mapJson(m_teamCounts) +
				",\n  \"classes\": " + mapJson(m_classCounts) +
				",\n  \"roles\": " + mapJson(m_roleCounts) +
				",\n  \"event_types\": " + mapJson(m_eventCounts) +
				",\n  \"files\": {\"events_jsonl_bytes\": " + fileSize("events.jsonl") +
				", \"snapshots_jsonl_bytes\": " + fileSize("snapshots.jsonl") + "}\n}\n";
			return result;
		}

		int count(const char* event) const
		{
			const auto it = m_eventCounts.find(event != nullptr ? event : "");
			return it == m_eventCounts.end() ? 0 : it->second;
		}

		int countPrefix(const char* prefix) const
		{
			if (prefix == nullptr)
				return 0;
			int total = 0;
			for (const auto& value : m_eventCounts)
				if (value.first.compare(0, std::strlen(prefix), prefix) == 0)
					total += value.second;
			return total;
		}

		std::string mapJson(const std::map<std::string, int>& values) const
		{
			std::string result = "{";
			bool first = true;
			for (const auto& value : values)
			{
				if (!first)
					result += ',';
				first = false;
				result += jsonString(value.first.c_str()) + ":" + std::to_string(value.second);
			}
			return result + "}";
		}

		std::string fileSize(const char* fileName) const
		{
			if (filesystem == nullptr || fileName == nullptr)
				return "0";
			return std::to_string(filesystem->Size((m_relativePath + "/" + fileName).c_str(), kPathId));
		}

		void setError(const char* error)
		{
			m_lastError = error != nullptr ? error : "unknown analysis recorder error";
			if (logger != nullptr)
				logger->Log(LogLevel::ERROR, "Empires analysis recorder error: %s", m_lastError.c_str());
		}

		void disable(const char* reason)
		{
			if (m_active)
			{
				m_active = false;
				setError(reason);
				closeFiles();
			}
		}

		bool m_active = false;
		FileHandle_t m_events = FILESYSTEM_INVALID_HANDLE;
		FileHandle_t m_snapshots = FILESYSTEM_INVALID_HANDLE;
		std::chrono::steady_clock::time_point m_start;
		double m_duration = 0.0;
		std::string m_sessionId;
		std::string m_relativePath;
		std::string m_map;
		std::string m_metadata;
		std::string m_lastError;
		std::string m_eventBuffer;
		std::string m_snapshotBuffer;
		std::map<int, BotRecord> m_bots;
		RCBotNavigationExperience::Store m_navigationExperience;
		std::set<RCBotNavigationExperience::TransitionKey> m_loggedLearnedEdges;
		std::uint64_t m_learnedNavigationBehaviorChanges = 0;
		std::uint64_t m_recoveryEpisodeBehaviorChanges = 0;
		std::uint64_t m_learnedEdgesWithPenalty = 0;
		double m_maxObservedPenalty = 0.0;
		double m_totalObservedPenalty = 0.0;
		std::uint64_t m_nonzeroPenaltyObservations = 0;
		RCBotLocalAvoidance::LocalConflictEpisodeTracker m_localConflictTracker;
		double m_localConflictLastScan = -1.0;
		std::uint64_t m_localConflictScanGeneration = 0;
		std::uint64_t m_localAvoidanceBehaviorChanges = 0;
		std::map<std::string, int> m_eventCounts;
		std::map<int, std::string> m_capturePointStates;
		std::map<int, EmpiresAnalysis::CommanderOrderObservation> m_commanderOrderObservations;
		std::map<std::string, int> m_teamCounts;
		std::map<std::string, int> m_classCounts;
		std::map<std::string, int> m_roleCounts;
		int m_eventCount = 0;
		int m_snapshotCount = 0;
		int m_botCount = 0;
		int m_userMarks = 0;
		int m_nextEventId = 0;
		int m_nextPathId = 0;
		int m_nextCommanderOrderId = 0;
	};

	CEmpiresAnalysisRecorder g_recorder;
}

namespace EmpiresAnalysis
{
	bool IsActive() { return g_recorder.isActive(); }
	bool Start(const char* mapName) { return g_recorder.start(mapName); }
	bool Stop() { return g_recorder.stop(); }
	void Mark(const char* text) { g_recorder.mark(text); }
	void RecordBotEvent(CBot* bot, const char* event, const edict_t* target,
		const char* detail) { g_recorder.recordBotEvent(bot, event, target, detail); }
	void RecordCapturePointState(const int entityIndex, const int pointIndex,
		const char* state)
	{
		g_recorder.recordCapturePointState(entityIndex, pointIndex, state);
	}
	void ObserveCommanderOrder(const CommanderOrderObservation& observation)
	{
		g_recorder.observeCommanderOrder(observation);
	}
	void RecordNativeCaptureEvent(const char* eventName, const bool team,
		const int location)
	{
		g_recorder.recordNativeCaptureEvent(eventName, team, location);
	}
	void RecordPathEvent(CBot* bot, const char* event, const int targetWaypoint,
		const char* detail) { g_recorder.recordPathEvent(bot, event, targetWaypoint, detail); }
	void RecordRecoveryEpisodeBegin(CBot* bot,
		const RCBotRecovery::RecoveryEpisodeIdentity& identity,
		const char* reason)
	{
		g_recorder.recordRecoveryEpisodeEvent(bot,
			"RECOVERY_EPISODE_BEGIN", identity, reason);
	}
	void RecordRecoveryEpisodeReuse(CBot* bot,
		const RCBotRecovery::RecoveryEpisodeIdentity& identity,
		const char* reason)
	{
		g_recorder.recordRecoveryEpisodeEvent(bot,
			"RECOVERY_EPISODE_REUSE", identity, reason);
	}
	void RecordRecoveryEpisodeProgress(CBot* bot, const char* reason)
	{
		g_recorder.recordRecoveryEpisodeProgress(bot, reason);
	}
	void RecordRecoveryEpisodeReplacement(CBot* bot,
		const RCBotRecovery::RecoveryEpisodeIdentity& identity,
		const char* reason)
	{
		g_recorder.recordRecoveryEpisodeEvent(bot,
			"RECOVERY_EPISODE_REPLACEMENT", identity, reason);
	}
	void RecordRecoveryEpisodeEnd(CBot* bot,
		const RCBotRecovery::RecoveryEpisodeIdentity& identity,
		const char* reason)
	{
		g_recorder.recordRecoveryEpisodeEvent(bot,
			"RECOVERY_EPISODE_END", identity, reason);
	}
	void RecordRecoveryPolicyEvent(CBot* bot, const char* event,
		const RCBotRecovery::RecoveryEpisodeIdentity& identity,
		const int consecutiveExhaustions, const char* reason)
	{
		g_recorder.recordRecoveryPolicyEvent(bot, event, identity,
			consecutiveExhaustions, reason);
	}
	void RecordNavigationAttemptStarted(CBot* bot,
		const RCBotNavigation::NavigationIntent& intent,
		const RCBotNavigation::NavigationAttempt& attempt)
	{
		g_recorder.recordNavigationAttemptStarted(bot, intent, attempt);
	}
	void RecordNavigationAttemptTerminal(CBot* bot,
		const RCBotNavigation::NavigationIntent& intent,
		const RCBotNavigation::NavigationAttempt& attempt,
		const RCBotNavigation::Result result, const char* reason)
	{
		g_recorder.recordNavigationAttemptTerminal(bot, intent, attempt,
			result, reason);
	}
	void RecordNavigationTransition(CBot* bot,
		const RCBotNavigation::NavigationIntent& intent,
		const RCBotNavigation::NavigationAttempt& attempt,
		const std::int64_t fromNode, const std::int64_t toNode,
		const RCBotNavigationExperience::Traversal traversal,
		const RCBotNavigationExperience::TransitionResult result,
		const RCBotNavigationExperience::CancellationKind cancellation,
		const char* reason)
	{
		g_recorder.recordNavigationTransition(bot, intent, attempt, fromNode,
			toNode, traversal, result, cancellation, reason);
	}
	void MarkNavigationTransitionStarted(CBot* bot,
		const RCBotNavigation::NavigationIntent& intent,
		const RCBotNavigation::NavigationAttempt& attempt,
		const std::int64_t fromNode, const std::int64_t toNode,
		const RCBotNavigationExperience::Traversal traversal)
	{
		g_recorder.markNavigationTransitionStarted(bot, intent, attempt,
			fromNode, toNode, traversal);
	}
	void CorrelateNavigationSignal(CBot* bot,
		const RCBotNavigationExperience::Signal signal)
	{
		g_recorder.correlateNavigationSignal(bot, signal);
	}
	RCBotNavigationExperience::LearnedNavigationCost EvaluateLearnedNavigationCost(
		const RCBotNavigationExperience::Provider provider,
		const std::int64_t fromNode, const std::int64_t toNode,
		const RCBotNavigationExperience::Traversal traversal)
	{
		return g_recorder.evaluateLearnedCost(provider, fromNode, toNode,
			traversal);
	}
	void RecordLearnedNavigationEdgeConsidered(CBot* bot,
		const RCBotNavigationExperience::Provider provider,
		const std::int64_t fromNode, const std::int64_t toNode,
		const RCBotNavigationExperience::Traversal traversal,
		const double baseCost, const double effectiveCost,
		const RCBotNavigationExperience::LearnedNavigationCost& cost)
	{
		g_recorder.recordLearnedEdgeConsidered(bot, provider, fromNode, toNode,
			traversal, baseCost, effectiveCost, cost);
	}
	void RecordLearnedNavigationBehaviorChange(CBot* bot,
		const RCBotNavigationExperience::Provider provider,
		const std::int64_t fromNode, const std::int64_t toNode,
		const RCBotNavigationExperience::Traversal traversal,
		const double baseCost, const double effectiveCost)
	{
		g_recorder.recordLearnedBehaviorChange(bot, provider, fromNode, toNode,
			traversal, baseCost, effectiveCost);
	}
	std::uint64_t LearnedNavigationBehaviorChanges()
	{
		return g_recorder.learnedBehaviorChanges();
	}
	std::uint64_t RecoveryEpisodeBehaviorChanges()
	{
		return g_recorder.recoveryEpisodeBehaviorChanges();
	}
	std::uint64_t LocalAvoidanceBehaviorChanges()
	{
		return g_recorder.localAvoidanceBehaviorChanges();
	}
	void RecordLocalYieldBehaviorChange(CBot* bot, const char* reason)
	{
		g_recorder.recordLocalYieldBehaviorChange(bot, reason);
	}
	int RecordCommanderEvent(CBot* bot, const char* event,
		const Vector& targetOrigin, const char* detail, const edict_t* target)
	{
		return g_recorder.recordCommanderEvent(bot, event, targetOrigin, detail, target);
	}
	void RecordOrderProtocolEvent(CBot* bot,
		const RCBotOrderTelemetry::TelemetryEvent& event)
	{
		g_recorder.recordOrderProtocolEvent(bot, event);
	}
	void RecordSquadShadowEvent(CBot* bot,
		const RCBotEmpiresSquadRuntime::Telemetry& event)
	{
		g_recorder.recordSquadShadowEvent(bot, event);
	}
	void ObserveBot(CBot* bot) { g_recorder.observeBot(bot); }
	const char* GetSessionId() { return g_recorder.sessionId(); }
	const char* GetSessionPath() { return g_recorder.sessionPath(); }
	const char* GetLastError() { return g_recorder.lastError(); }
	int GetEventCount() { return g_recorder.eventCount(); }
	int GetBotCount() { return g_recorder.botCount(); }
	double GetDurationSeconds() { return g_recorder.duration(); }
}
