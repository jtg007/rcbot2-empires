#ifndef __RCBOT_LOCAL_EXPLORER_H__
#define __RCBOT_LOCAL_EXPLORER_H__

#include <cstdint>

#include "vector.h"

class CBot;

namespace RCBotLocalExplorer
{
enum class Result : std::uint8_t
{
	NONE = 0,
	EXPLORING,
	LOCAL_PATH_FOUND,
	LOCAL_UNREACHABLE,
	LOCAL_BUDGET_EXHAUSTED,
	CANCELLED,
	LOCAL_SEGMENT_FAILED,
	COMPLETE
};

struct Config
{
	float radius = 384.0f;
	float cellSize = 32.0f;
	int maxNodes = 192;
	int maxProbes = 2048;
	int maxExpansions = 512;
	int maxWorkPerFrame = 12;
	float maxDuration = 12.0f;
};

struct Status
{
	bool active = false;
	Result result = Result::NONE;
	std::uint64_t sessionId = 0;
	std::uint64_t intentId = 0;
	std::uint64_t attemptId = 0;
	int goalId = -1;
	int nodes = 0;
	int probes = 0;
	int expansions = 0;
	int pathPoints = 0;
	int segment = 0;
	int walkEdges = 0;
	int stepUpEdges = 0;
	int stepDownEdges = 0;
	int frames = 0;
};

const char *ResultName(Result result);

// Starts an explicit, test-only local exploration. This creates its own
// intent because it is outside provider arbitration.
bool Start(CBot *bot, const Vector &goal, const Config &config = Config());
// Starts the explorer as the current provider of an existing AUTO intent.
// The adapter adopts the intent/goal and creates only a provider-specific
// attempt; it must never replace the intent created by the preceding provider.
bool StartForIntent(CBot *bot, std::uint64_t intentId, const Vector &goal,
	const Config &config = Config());
bool Cancel(CBot *bot, const char *reason);
bool IsActive(const CBot *bot);
bool GetStatus(const CBot *bot, Status *status);

// Called after CBot::think() so the normal doMove()/RunPlayerMove boundary
// consumes the target on the following frame.
void Frame();
void LevelShutdown();
}

#endif // __RCBOT_LOCAL_EXPLORER_H__
