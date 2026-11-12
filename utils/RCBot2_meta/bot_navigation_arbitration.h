#ifndef __RCBOT_NAVIGATION_ARBITRATION_H__
#define __RCBOT_NAVIGATION_ARBITRATION_H__

#include "bot_navigation_attempt.h"

namespace RCBotNavigation
{
const char* NavigationPolicyName(NavigationPolicy policy);
const char* FailureClassName(FailureClass classification);
const char* FailureReasonName(FailureReason reason);

// These selectors deliberately inspect only a policy, the current provider
// and two bits of provider history. They never run pathfinding.
Provider FirstProvider(NavigationPolicy policy);
bool IsProviderAllowed(NavigationPolicy policy, Provider provider);
Provider NextProvider(NavigationPolicy policy, Provider current,
	std::uint8_t providersAttempted, FailureClass failureClass);
}

#endif // __RCBOT_NAVIGATION_ARBITRATION_H__
