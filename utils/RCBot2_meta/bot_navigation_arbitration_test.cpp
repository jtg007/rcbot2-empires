#include <cassert>
#include <iostream>

#include "bot_navigation_arbitration.h"

using namespace RCBotNavigation;

int main()
{
	assert(FirstProvider(NavigationPolicy::AUTO) == Provider::WAYPOINT);
	assert(FirstProvider(NavigationPolicy::WAYPOINT_ONLY) == Provider::WAYPOINT);
	assert(FirstProvider(NavigationPolicy::NAVMESH_ONLY) == Provider::NAVMESH);
	assert(IsProviderAllowed(NavigationPolicy::AUTO, Provider::WAYPOINT));
	assert(IsProviderAllowed(NavigationPolicy::AUTO, Provider::NAVMESH));
	// Local Explorer is reached only through AUTO's bounded fallback adapter;
	// it is not a separately selectable policy in this phase.
	assert(!IsProviderAllowed(NavigationPolicy::AUTO, Provider::LOCAL_EXPLORER));
	assert(!IsProviderAllowed(NavigationPolicy::WAYPOINT_ONLY, Provider::NAVMESH));
	assert(!IsProviderAllowed(NavigationPolicy::NAVMESH_ONLY, Provider::WAYPOINT));

	const std::uint8_t waypointOnly = ProviderBit(Provider::WAYPOINT);
	assert(NextProvider(NavigationPolicy::AUTO, Provider::WAYPOINT,
		waypointOnly, FailureClass::STRUCTURAL_PLANNING) == Provider::NAVMESH);
	assert(NextProvider(NavigationPolicy::AUTO, Provider::WAYPOINT,
		waypointOnly, FailureClass::MOVEMENT) == Provider::UNKNOWN);
	assert(NextProvider(NavigationPolicy::AUTO, Provider::WAYPOINT,
		waypointOnly, FailureClass::CANCEL_INVALID) == Provider::UNKNOWN);
	assert(NextProvider(NavigationPolicy::AUTO, Provider::WAYPOINT,
		waypointOnly | ProviderBit(Provider::NAVMESH),
		FailureClass::STRUCTURAL_PLANNING) == Provider::UNKNOWN);
	const std::uint8_t waypointAndNavMesh = waypointOnly |
		ProviderBit(Provider::NAVMESH);
	assert(NextProvider(NavigationPolicy::AUTO, Provider::NAVMESH,
		waypointAndNavMesh, FailureClass::STRUCTURAL_PLANNING) ==
		Provider::LOCAL_EXPLORER);
	assert(NextProvider(NavigationPolicy::AUTO, Provider::NAVMESH,
		waypointAndNavMesh | ProviderBit(Provider::LOCAL_EXPLORER),
		FailureClass::STRUCTURAL_PLANNING) == Provider::UNKNOWN);
	assert(NextProvider(NavigationPolicy::AUTO, Provider::NAVMESH,
		waypointAndNavMesh, FailureClass::MOVEMENT) == Provider::UNKNOWN);
	assert(NextProvider(NavigationPolicy::AUTO, Provider::LOCAL_EXPLORER,
		waypointAndNavMesh, FailureClass::STRUCTURAL_PLANNING) ==
		Provider::UNKNOWN);
	assert(NextProvider(NavigationPolicy::WAYPOINT_ONLY, Provider::WAYPOINT,
		waypointOnly, FailureClass::STRUCTURAL_PLANNING) == Provider::UNKNOWN);

	NavigationIntent intent;
	intent.active = true;
	intent.policy = NavigationPolicy::AUTO;
	intent.provider = Provider::WAYPOINT;
	intent.MarkProviderAttempted(Provider::WAYPOINT);
	assert(!intent.HasAttemptedProvider(Provider::NAVMESH));
	intent.RecordProviderFailure(Provider::WAYPOINT,
		FailureClass::STRUCTURAL_PLANNING, FailureReason::NO_REACHABLE_ANCHOR,
		Result::UNREACHABLE);
	assert(intent.failureReasonPerProvider[0].recorded);
	assert(intent.failureReasonPerProvider[0].reason ==
		FailureReason::NO_REACHABLE_ANCHOR);
	intent.MarkProviderAttempted(Provider::NAVMESH);
	assert(intent.HasAttemptedProvider(Provider::NAVMESH));
	assert(!intent.HasAttemptedProvider(Provider::LOCAL_EXPLORER));
	intent.MarkProviderAttempted(Provider::LOCAL_EXPLORER);
	assert(intent.providersAttempted ==
		(ProviderBit(Provider::WAYPOINT) | ProviderBit(Provider::NAVMESH) |
			ProviderBit(Provider::LOCAL_EXPLORER)));
	intent.RecordProviderFailure(Provider::LOCAL_EXPLORER,
		FailureClass::STRUCTURAL_PLANNING, FailureReason::NO_PATH,
		Result::UNREACHABLE);
	assert(intent.failureReasonPerProvider[2].recorded);
	assert(intent.failureReasonPerProvider[2].reason == FailureReason::NO_PATH);

	std::cout << "policy and bounded three-provider fallback PASS" << std::endl;
	return 0;
}
