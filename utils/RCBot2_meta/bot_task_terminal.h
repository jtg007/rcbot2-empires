// Diagnostic-only task terminal cause shared by task producers (bot_task.*,
// bot_schedule.cpp) and read-only consumers (per-bot slot on CBot, Empires
// capture-route diagnostics).
//
// This header has no dependencies so both bot.h and bot_task.h can include
// it without creating an include cycle. Values are factual terminal paths;
// see CBotTask::setTerminalCause call sites. NEVER branch production behavior
// on these values.
#ifndef BOT_TASK_TERMINAL_H
#define BOT_TASK_TERMINAL_H

enum class TaskTerminalCause : unsigned char
{
	UNKNOWN = 0,
	// Generic schedule-level terminals (bot_schedule.cpp).
	INTERRUPTED,
	TIMED_OUT,
	// CFindPathTask terminals.
	FIND_PATH_INTENT_REJECTED,
	FIND_PATH_ANCHOR_NOT_READY,
	FIND_PATH_ATTEMPT_REJECTED,
	FIND_PATH_NAVIGATOR_FAILURE,
	FIND_PATH_DIRECT_FALLBACK,
	FIND_PATH_NO_PROGRESS_TIMEOUT,
	FIND_PATH_STUCK_SENTINEL,
	FIND_PATH_GOAL_REACHED,
	FIND_PATH_TERMINAL_COMPLETE,
	FIND_PATH_TERMINAL_FAILED,
	FIND_PATH_INTENT_CLEARED,
	FIND_PATH_PASSED_INVALID,
	FIND_PATH_WAYPOINT_NOT_FOUND,
	// CMoveToTask terminals.
	MOVE_TARGET_INVALID,
	MOVE_NO_PROGRESS_TIMEOUT,
	MOVE_STUCK_SENTINEL,
	MOVE_REACHED
};

inline const char* TaskTerminalCauseName(const TaskTerminalCause cause)
{
	switch (cause)
	{
	case TaskTerminalCause::UNKNOWN:
		return "UNKNOWN";
	case TaskTerminalCause::INTERRUPTED:
		return "INTERRUPTED";
	case TaskTerminalCause::TIMED_OUT:
		return "TIMED_OUT";
	case TaskTerminalCause::FIND_PATH_INTENT_REJECTED:
		return "FIND_PATH_INTENT_REJECTED";
	case TaskTerminalCause::FIND_PATH_ANCHOR_NOT_READY:
		return "FIND_PATH_ANCHOR_NOT_READY";
	case TaskTerminalCause::FIND_PATH_ATTEMPT_REJECTED:
		return "FIND_PATH_ATTEMPT_REJECTED";
	case TaskTerminalCause::FIND_PATH_NAVIGATOR_FAILURE:
		return "FIND_PATH_NAVIGATOR_FAILURE";
	case TaskTerminalCause::FIND_PATH_DIRECT_FALLBACK:
		return "FIND_PATH_DIRECT_FALLBACK";
	case TaskTerminalCause::FIND_PATH_NO_PROGRESS_TIMEOUT:
		return "FIND_PATH_NO_PROGRESS_TIMEOUT";
	case TaskTerminalCause::FIND_PATH_STUCK_SENTINEL:
		return "FIND_PATH_STUCK_SENTINEL";
	case TaskTerminalCause::FIND_PATH_GOAL_REACHED:
		return "FIND_PATH_GOAL_REACHED";
	case TaskTerminalCause::FIND_PATH_TERMINAL_COMPLETE:
		return "FIND_PATH_TERMINAL_COMPLETE";
	case TaskTerminalCause::FIND_PATH_TERMINAL_FAILED:
		return "FIND_PATH_TERMINAL_FAILED";
	case TaskTerminalCause::FIND_PATH_INTENT_CLEARED:
		return "FIND_PATH_INTENT_CLEARED";
	case TaskTerminalCause::FIND_PATH_PASSED_INVALID:
		return "FIND_PATH_PASSED_INVALID";
	case TaskTerminalCause::FIND_PATH_WAYPOINT_NOT_FOUND:
		return "FIND_PATH_WAYPOINT_NOT_FOUND";
	case TaskTerminalCause::MOVE_TARGET_INVALID:
		return "MOVE_TARGET_INVALID";
	case TaskTerminalCause::MOVE_NO_PROGRESS_TIMEOUT:
		return "MOVE_NO_PROGRESS_TIMEOUT";
	case TaskTerminalCause::MOVE_STUCK_SENTINEL:
		return "MOVE_STUCK_SENTINEL";
	case TaskTerminalCause::MOVE_REACHED:
		return "MOVE_REACHED";
	}
	return "UNKNOWN";
}

#endif // BOT_TASK_TERMINAL_H
