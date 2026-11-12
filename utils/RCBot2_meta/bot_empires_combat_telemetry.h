#ifndef _BOT_EMPIRES_COMBAT_TELEMETRY_H_
#define _BOT_EMPIRES_COMBAT_TELEMETRY_H_

#include <cstdint>
#include <cstdio>
#include <string>

// Formatting-only helpers for the existing Empires analysis events.  These
// helpers deliberately own no gameplay state and make no decisions.
namespace RCBotEmpiresCombatTelemetry
{

struct Correlation
{
	double curtime = 0.0;
	std::uint64_t lifeGeneration = 0;
	std::uint64_t captureGeneration = 0;
	std::uint64_t captureAttempt = 0;
	int ownerUserId = -1;
	int ownerTeam = -1;
	int capturePoint = -1;
	int targetIndex = -1;
	int targetUserId = -1;
	int targetTeam = -1;
};

inline std::string FormatCorrelation(const Correlation& value)
{
	char buffer[512];
	std::snprintf(buffer, sizeof(buffer),
		"curtime=%.3f;life_generation=%llu;capture_generation=%llu;"
		"capture_attempt=%llu;owner_userid=%d;owner_team=%d;"
		"capture_point=%d;target_index=%d;target_userid=%d;target_team=%d",
		value.curtime,
		static_cast<unsigned long long>(value.lifeGeneration),
		static_cast<unsigned long long>(value.captureGeneration),
		static_cast<unsigned long long>(value.captureAttempt),
		value.ownerUserId, value.ownerTeam, value.capturePoint,
		value.targetIndex, value.targetUserId, value.targetTeam);
	return buffer;
}

inline std::string FormatDecision(const char* phase, const char* decision,
	const char* reason, const char* callerOutcome)
{
	char buffer[256];
	std::snprintf(buffer, sizeof(buffer),
		"decision_phase=%s;decision=%s;decision_reason=%s;caller_outcome=%s",
		phase != nullptr ? phase : "unknown",
		decision != nullptr ? decision : "unknown",
		reason != nullptr ? reason : "unknown",
		callerOutcome != nullptr ? callerOutcome : "unknown");
	return buffer;
}

inline std::string FormatRetryAccounting(const bool latchBefore,
	const bool objectiveValidBefore, const int retryBefore,
	const int retryAfter, const bool combatResume, const bool consumeRetry)
{
	char buffer[256];
	std::snprintf(buffer, sizeof(buffer),
		"combat_latch_before=%d;capture_valid_before=%d;retry_before=%d;"
		"retry_after=%d;combat_resume=%d;consume_retry=%d",
		latchBefore ? 1 : 0, objectiveValidBefore ? 1 : 0, retryBefore,
		retryAfter, combatResume ? 1 : 0, consumeRetry ? 1 : 0);
	return buffer;
}

} // namespace RCBotEmpiresCombatTelemetry

#endif
