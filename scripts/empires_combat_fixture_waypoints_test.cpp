#include "../utils/RCBot2_meta/bot_empires_combat_fixture_waypoints.h"

int main()
{
	EmpiresCombatFixture::WaypointSet waypoints{ -1, -1, -1 };

	if (!EmpiresCombatFixture::selectWaypointSet(95, &waypoints) ||
		waypoints.observer != 7 || waypoints.opponentStart != 10 ||
		waypoints.opponentDestination != 8)
	{
		return 1;
	}

	if (!EmpiresCombatFixture::selectWaypointSet(219, &waypoints) ||
		waypoints.observer != 147 || waypoints.opponentStart != 149 ||
		waypoints.opponentDestination != 156)
	{
		return 2;
	}

	if (!EmpiresCombatFixture::selectWaypointSet(157, &waypoints) ||
		waypoints.observer != 147 || waypoints.opponentStart != 149 ||
		waypoints.opponentDestination != 156)
	{
		return 3;
	}

	if (EmpiresCombatFixture::selectWaypointSet(120, &waypoints) ||
		EmpiresCombatFixture::selectWaypointSet(94, &waypoints) ||
		EmpiresCombatFixture::selectWaypointSet(95, nullptr))
	{
		return 4;
	}

	return 0;
}
