// Event-driven capture-route terminal diagnostics: engine-independent
// classification for capture routes that drain before arrival.
//
// When a capture approach schedule empties while the bot is still far from
// the objective, the retry branch must decide: consume a bounded retry,
// resume for free after combat, or reject. Previously the only evidence was
// the remaining distance, which cannot distinguish an instant path failure
// from a stuck move task. This header maps the factual per-task terminal
// cause (TaskTerminalCause, recorded read-only by the schedule executor)
// to a small diagnostic terminal and to the zero-progress episode
// classification used by the Objective Progression Audit.
//
// This header owns no schedules, timers, navigation, or combat policy and
// never affects behavior; it only classifies already-recorded facts. Unknown
// or unavailable causes map to UNKNOWN, never to a guessed reason.
#ifndef BOT_EMPIRES_CAPTURE_ROUTE_TERMINAL_H
#define BOT_EMPIRES_CAPTURE_ROUTE_TERMINAL_H

#include "bot_task_terminal.h"

namespace RCBotEmpiresCaptureRouteTerminal
{

enum class RouteTerminal
{
	UNKNOWN = 0,
	FIND_PATH_FAILED,
	FIND_PATH_COMPLETED,
	MOVE_COMPLETED,
	MOVE_STUCK_FAILED,
	TASK_REPLACED_CANCELLED
};

inline const char* RouteTerminalName(const RouteTerminal terminal)
{
	switch (terminal)
	{
	case RouteTerminal::UNKNOWN:
		return "UNKNOWN";
	case RouteTerminal::FIND_PATH_FAILED:
		return "FIND_PATH_FAILED";
	case RouteTerminal::FIND_PATH_COMPLETED:
		return "FIND_PATH_COMPLETED";
	case RouteTerminal::MOVE_COMPLETED:
		return "MOVE_COMPLETED";
	case RouteTerminal::MOVE_STUCK_FAILED:
		return "MOVE_STUCK_FAILED";
	case RouteTerminal::TASK_REPLACED_CANCELLED:
		return "TASK_REPLACED_CANCELLED";
	}
	return "UNKNOWN";
}

// Maps a factual task terminal cause to the diagnostic terminal. Combat
// preemption is NOT represented here: it is attributed separately via the
// capture-interruption latch, which takes precedence over this mapping.
inline RouteTerminal MapTaskTerminalCause(const TaskTerminalCause cause)
{
	switch (cause)
	{
	case TaskTerminalCause::FIND_PATH_INTENT_REJECTED:
	case TaskTerminalCause::FIND_PATH_ANCHOR_NOT_READY:
	case TaskTerminalCause::FIND_PATH_ATTEMPT_REJECTED:
	case TaskTerminalCause::FIND_PATH_NAVIGATOR_FAILURE:
	case TaskTerminalCause::FIND_PATH_TERMINAL_FAILED:
	case TaskTerminalCause::FIND_PATH_INTENT_CLEARED:
	case TaskTerminalCause::FIND_PATH_PASSED_INVALID:
	case TaskTerminalCause::FIND_PATH_WAYPOINT_NOT_FOUND:
		return RouteTerminal::FIND_PATH_FAILED;
	case TaskTerminalCause::FIND_PATH_DIRECT_FALLBACK:
	case TaskTerminalCause::FIND_PATH_GOAL_REACHED:
	case TaskTerminalCause::FIND_PATH_TERMINAL_COMPLETE:
		return RouteTerminal::FIND_PATH_COMPLETED;
	case TaskTerminalCause::MOVE_REACHED:
		return RouteTerminal::MOVE_COMPLETED;
	case TaskTerminalCause::MOVE_TARGET_INVALID:
	case TaskTerminalCause::MOVE_NO_PROGRESS_TIMEOUT:
	case TaskTerminalCause::MOVE_STUCK_SENTINEL:
	case TaskTerminalCause::FIND_PATH_NO_PROGRESS_TIMEOUT:
	case TaskTerminalCause::FIND_PATH_STUCK_SENTINEL:
		return RouteTerminal::MOVE_STUCK_FAILED;
	case TaskTerminalCause::INTERRUPTED:
	case TaskTerminalCause::TIMED_OUT:
		return RouteTerminal::TASK_REPLACED_CANCELLED;
	case TaskTerminalCause::UNKNOWN:
		return RouteTerminal::UNKNOWN;
	}
	return RouteTerminal::UNKNOWN;
}

// Zero-progress episode classification (Objective Progression Audit A-I).
// The pure terminal mapping above answers most cases; E (blocking) and D
// (recovery) require the stuck/recovery fields carried by the event and are
// classified manually from a full episode.
enum class ZeroProgressCause
{
	A_FIND_PATH_FAILED_BEFORE_MOVEMENT = 0,
	B_PATH_BUILT_BUT_MOVE_NEVER_STARTED,
	C_MOVE_STARTED_BUT_STUCK,
	D_MOVE_TERMINATED_BY_RECOVERY,
	E_LOCAL_BOT_BLOCKING,
	F_PROVIDER_GRAPH_FAILURE,
	G_TASK_REPLACED_CANCELLED,
	H_OTHER,
	I_UNKNOWN
};

inline const char* ZeroProgressCauseName(const ZeroProgressCause cause)
{
	switch (cause)
	{
	case ZeroProgressCause::A_FIND_PATH_FAILED_BEFORE_MOVEMENT:
		return "A_FIND_PATH_FAILED_BEFORE_MOVEMENT";
	case ZeroProgressCause::B_PATH_BUILT_BUT_MOVE_NEVER_STARTED:
		return "B_PATH_BUILT_BUT_MOVE_NEVER_STARTED";
	case ZeroProgressCause::C_MOVE_STARTED_BUT_STUCK:
		return "C_MOVE_STARTED_BUT_STUCK";
	case ZeroProgressCause::D_MOVE_TERMINATED_BY_RECOVERY:
		return "D_MOVE_TERMINATED_BY_RECOVERY";
	case ZeroProgressCause::E_LOCAL_BOT_BLOCKING:
		return "E_LOCAL_BOT_BLOCKING";
	case ZeroProgressCause::F_PROVIDER_GRAPH_FAILURE:
		return "F_PROVIDER_GRAPH_FAILURE";
	case ZeroProgressCause::G_TASK_REPLACED_CANCELLED:
		return "G_TASK_REPLACED_CANCELLED";
	case ZeroProgressCause::H_OTHER:
		return "H_OTHER";
	case ZeroProgressCause::I_UNKNOWN:
		return "I_UNKNOWN";
	}
	return "I_UNKNOWN";
}

// Suggests the episode classification supported by the terminal alone.
// FIND_PATH_FAILED covers both A (immediate failure) and F (graph failure);
// the event's displacement/provider/waypoint fields separate them.
inline ZeroProgressCause SuggestCauseForTerminal(
	const RouteTerminal terminal)
{
	switch (terminal)
	{
	case RouteTerminal::FIND_PATH_FAILED:
		return ZeroProgressCause::A_FIND_PATH_FAILED_BEFORE_MOVEMENT;
	case RouteTerminal::FIND_PATH_COMPLETED:
		return ZeroProgressCause::B_PATH_BUILT_BUT_MOVE_NEVER_STARTED;
	case RouteTerminal::MOVE_COMPLETED:
		return ZeroProgressCause::H_OTHER;
	case RouteTerminal::MOVE_STUCK_FAILED:
		return ZeroProgressCause::C_MOVE_STARTED_BUT_STUCK;
	case RouteTerminal::TASK_REPLACED_CANCELLED:
		return ZeroProgressCause::G_TASK_REPLACED_CANCELLED;
	case RouteTerminal::UNKNOWN:
		return ZeroProgressCause::I_UNKNOWN;
	}
	return ZeroProgressCause::I_UNKNOWN;
}

} // namespace RCBotEmpiresCaptureRouteTerminal

#endif // BOT_EMPIRES_CAPTURE_ROUTE_TERMINAL_H
