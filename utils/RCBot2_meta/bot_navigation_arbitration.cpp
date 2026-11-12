#include "bot_navigation_arbitration.h"

namespace RCBotNavigation
{
const char* NavigationPolicyName(const NavigationPolicy policy)
{
	switch (policy)
	{
	case NavigationPolicy::AUTO: return "AUTO";
	case NavigationPolicy::WAYPOINT_ONLY: return "WAYPOINT_ONLY";
	case NavigationPolicy::NAVMESH_ONLY: return "NAVMESH_ONLY";
	}
	return "UNKNOWN";
}

const char* FailureClassName(const FailureClass classification)
{
	switch (classification)
	{
	case FailureClass::STRUCTURAL_PLANNING: return "STRUCTURAL_PLANNING";
	case FailureClass::MOVEMENT: return "MOVEMENT";
	case FailureClass::CANCEL_INVALID: return "CANCEL_INVALID";
	default: return "NONE";
	}
}

const char* FailureReasonName(const FailureReason reason)
{
	switch (reason)
	{
	case FailureReason::NO_START_ANCHOR: return "NO_START_ANCHOR";
	case FailureReason::NO_GOAL_ANCHOR: return "NO_GOAL_ANCHOR";
	case FailureReason::NO_REACHABLE_ANCHOR: return "NO_REACHABLE_ANCHOR";
	case FailureReason::START_AREA_NOT_FOUND: return "START_AREA_NOT_FOUND";
	case FailureReason::GOAL_AREA_NOT_FOUND: return "GOAL_AREA_NOT_FOUND";
	case FailureReason::NO_PATH: return "NO_PATH";
	case FailureReason::PROVIDER_UNAVAILABLE: return "PROVIDER_UNAVAILABLE";
	case FailureReason::PATH_FAILED: return "PATH_FAILED";
	case FailureReason::PATH_STUCK: return "PATH_STUCK";
	case FailureReason::REPEATED_LOCOMOTION_FAILURE:
		return "REPEATED_LOCOMOTION_FAILURE";
	case FailureReason::CANCELLED: return "CANCELLED";
	case FailureReason::DEATH: return "DEATH";
	case FailureReason::TEAM_CHANGED: return "TEAM_CHANGED";
	case FailureReason::INVALID_INTENT: return "INVALID_INTENT";
	case FailureReason::EXPLICIT_HARD_STOP: return "EXPLICIT_HARD_STOP";
	case FailureReason::STALE_OUTPUT: return "STALE_OUTPUT";
	default: return "NONE";
	}
}

Provider FirstProvider(const NavigationPolicy policy)
{
	return policy == NavigationPolicy::NAVMESH_ONLY ?
		Provider::NAVMESH : Provider::WAYPOINT;
}

bool IsProviderAllowed(const NavigationPolicy policy, const Provider provider)
{
	if (provider != Provider::WAYPOINT && provider != Provider::NAVMESH)
		return false;
	return policy == NavigationPolicy::AUTO ||
		(policy == NavigationPolicy::WAYPOINT_ONLY &&
			provider == Provider::WAYPOINT) ||
		(policy == NavigationPolicy::NAVMESH_ONLY &&
			provider == Provider::NAVMESH);
}

Provider NextProvider(const NavigationPolicy policy, const Provider current,
	const std::uint8_t providersAttempted, const FailureClass failureClass)
{
	if (failureClass != FailureClass::STRUCTURAL_PLANNING ||
		policy != NavigationPolicy::AUTO)
		return Provider::UNKNOWN;

	if (current == Provider::WAYPOINT)
	{
		if ((providersAttempted & ProviderBit(Provider::NAVMESH)) != 0)
			return Provider::UNKNOWN;
		return Provider::NAVMESH;
	}
	if (current == Provider::NAVMESH)
	{
		if ((providersAttempted & ProviderBit(Provider::LOCAL_EXPLORER)) != 0)
			return Provider::UNKNOWN;
		return Provider::LOCAL_EXPLORER;
	}
	// Local Explorer is the last bounded provider.  In particular, a local
	// budget or path result must never restart the explorer or ping-pong back.
	return Provider::UNKNOWN;
}
}
