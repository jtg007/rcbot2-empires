// Deterministic coverage for capture-route terminal classification.
//
// These tests exercise REAL production code
// (RCBotEmpiresCaptureRouteTerminal::MapTaskTerminalCause,
// SuggestCauseForTerminal, RouteTerminalName, ZeroProgressCauseName,
// TaskTerminalCauseName), never a copy of the policy. The engine-bound
// wiring (per-bot terminal slot, schedule-executor notes, CAPTURE_ROUTE_
// TERMINAL emission) is reviewable in bot.h / bot.cpp / bot_schedule.cpp /
// bot_task.cpp / bot_empires_objective.cpp and verified live via the
// official harness observation; the classification decision itself is fully
// covered here.
//
// Required cases:
// - TERMINAL-T1 every TaskTerminalCause maps to exactly one RouteTerminal.
// - TERMINAL-T2 unknown/unavailable cause maps to UNKNOWN, never guessed.
// - TERMINAL-T3 interrupt/timeout maps to TASK_REPLACED_CANCELLED.
// - TERMINAL-T4 all find-path failure variants map to FIND_PATH_FAILED.
// - TERMINAL-T5 move stuck variants map to MOVE_STUCK_FAILED.
// - TERMINAL-T6 completion variants map to their completed terminals.
// - TERMINAL-T7 suggested zero-progress cause follows the terminal.

#include "bot_empires_capture_route_terminal.h"

#include <cstdlib>
#include <iostream>

namespace
{

using namespace RCBotEmpiresCaptureRouteTerminal;

void Require(const bool condition, const char* message)
{
	if (!condition)
	{
		std::cerr << "FAIL: " << message << std::endl;
		std::exit(1);
	}
}

void CheckMapping(const TaskTerminalCause cause,
	const RouteTerminal expected, const char* message)
{
	Require(MapTaskTerminalCause(cause) == expected, message);
}

} // namespace

int main()
{
	// TERMINAL-T1/T2: exhaustive mapping, UNKNOWN default.
	CheckMapping(TaskTerminalCause::UNKNOWN, RouteTerminal::UNKNOWN,
		"TERMINAL-T2 unknown maps to unknown");
	Require(RouteTerminalName(RouteTerminal::UNKNOWN) != nullptr,
		"terminal names exist");
	Require(TaskTerminalCauseName(TaskTerminalCause::UNKNOWN) != nullptr,
		"cause names exist");
	Require(ZeroProgressCauseName(ZeroProgressCause::I_UNKNOWN) != nullptr,
		"suggested names exist");

	// TERMINAL-T3: generic schedule terminals.
	CheckMapping(TaskTerminalCause::INTERRUPTED,
		RouteTerminal::TASK_REPLACED_CANCELLED,
		"TERMINAL-T3 interrupted maps to replaced/cancelled");
	CheckMapping(TaskTerminalCause::TIMED_OUT,
		RouteTerminal::TASK_REPLACED_CANCELLED,
		"TERMINAL-T3 timed out maps to replaced/cancelled");

	// TERMINAL-T4: every find-path failure variant.
	CheckMapping(TaskTerminalCause::FIND_PATH_INTENT_REJECTED,
		RouteTerminal::FIND_PATH_FAILED, "TERMINAL-T4 intent rejected");
	CheckMapping(TaskTerminalCause::FIND_PATH_ANCHOR_NOT_READY,
		RouteTerminal::FIND_PATH_FAILED, "TERMINAL-T4 anchor not ready");
	CheckMapping(TaskTerminalCause::FIND_PATH_ATTEMPT_REJECTED,
		RouteTerminal::FIND_PATH_FAILED, "TERMINAL-T4 attempt rejected");
	CheckMapping(TaskTerminalCause::FIND_PATH_NAVIGATOR_FAILURE,
		RouteTerminal::FIND_PATH_FAILED, "TERMINAL-T4 navigator failure");
	CheckMapping(TaskTerminalCause::FIND_PATH_TERMINAL_FAILED,
		RouteTerminal::FIND_PATH_FAILED, "TERMINAL-T4 terminal failed");
	CheckMapping(TaskTerminalCause::FIND_PATH_INTENT_CLEARED,
		RouteTerminal::FIND_PATH_FAILED, "TERMINAL-T4 intent cleared");
	CheckMapping(TaskTerminalCause::FIND_PATH_PASSED_INVALID,
		RouteTerminal::FIND_PATH_FAILED, "TERMINAL-T4 passed invalid");
	CheckMapping(TaskTerminalCause::FIND_PATH_WAYPOINT_NOT_FOUND,
		RouteTerminal::FIND_PATH_FAILED, "TERMINAL-T4 waypoint not found");

	// TERMINAL-T5: move stuck variants.
	CheckMapping(TaskTerminalCause::MOVE_TARGET_INVALID,
		RouteTerminal::MOVE_STUCK_FAILED, "TERMINAL-T5 target invalid");
	CheckMapping(TaskTerminalCause::MOVE_NO_PROGRESS_TIMEOUT,
		RouteTerminal::MOVE_STUCK_FAILED, "TERMINAL-T5 no progress");
	CheckMapping(TaskTerminalCause::MOVE_STUCK_SENTINEL,
		RouteTerminal::MOVE_STUCK_FAILED, "TERMINAL-T5 stuck sentinel");
	CheckMapping(TaskTerminalCause::FIND_PATH_NO_PROGRESS_TIMEOUT,
		RouteTerminal::MOVE_STUCK_FAILED,
		"TERMINAL-T5 path no-progress timeout");
	CheckMapping(TaskTerminalCause::FIND_PATH_STUCK_SENTINEL,
		RouteTerminal::MOVE_STUCK_FAILED,
		"TERMINAL-T5 path stuck sentinel");

	// TERMINAL-T6: completion variants.
	CheckMapping(TaskTerminalCause::FIND_PATH_DIRECT_FALLBACK,
		RouteTerminal::FIND_PATH_COMPLETED, "TERMINAL-T6 direct fallback");
	CheckMapping(TaskTerminalCause::FIND_PATH_GOAL_REACHED,
		RouteTerminal::FIND_PATH_COMPLETED, "TERMINAL-T6 goal reached");
	CheckMapping(TaskTerminalCause::FIND_PATH_TERMINAL_COMPLETE,
		RouteTerminal::FIND_PATH_COMPLETED, "TERMINAL-T6 terminal complete");
	CheckMapping(TaskTerminalCause::MOVE_REACHED,
		RouteTerminal::MOVE_COMPLETED, "TERMINAL-T6 move reached");

	// TERMINAL-T7: suggested cause follows the terminal.
	Require(SuggestCauseForTerminal(RouteTerminal::FIND_PATH_FAILED) ==
		ZeroProgressCause::A_FIND_PATH_FAILED_BEFORE_MOVEMENT,
		"TERMINAL-T7 failed suggests A");
	Require(SuggestCauseForTerminal(RouteTerminal::FIND_PATH_COMPLETED) ==
		ZeroProgressCause::B_PATH_BUILT_BUT_MOVE_NEVER_STARTED,
		"TERMINAL-T7 path-complete suggests B");
	Require(SuggestCauseForTerminal(RouteTerminal::MOVE_STUCK_FAILED) ==
		ZeroProgressCause::C_MOVE_STARTED_BUT_STUCK,
		"TERMINAL-T7 stuck suggests C");
	Require(SuggestCauseForTerminal(
		RouteTerminal::TASK_REPLACED_CANCELLED) ==
		ZeroProgressCause::G_TASK_REPLACED_CANCELLED,
		"TERMINAL-T7 replaced suggests G");
	Require(SuggestCauseForTerminal(RouteTerminal::UNKNOWN) ==
		ZeroProgressCause::I_UNKNOWN, "TERMINAL-T7 unknown suggests I");
	Require(SuggestCauseForTerminal(RouteTerminal::MOVE_COMPLETED) ==
		ZeroProgressCause::H_OTHER, "TERMINAL-T7 move-complete is other");

	std::cout << "capture-route-terminal tests passed" << std::endl;
	return 0;
}
