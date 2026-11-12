#ifndef _BOT_EMPIRES_COMBAT_FIXTURE_WAYPOINTS_H_
#define _BOT_EMPIRES_COMBAT_FIXTURE_WAYPOINTS_H_

namespace EmpiresCombatFixture
{
struct WaypointSet
{
	int observer;
	int opponentStart;
	int opponentDestination;
};

// con_district402 has a current 95-waypoint graph and an older graph that
// contains the originally verified combat positions. Unknown graph sizes are
// rejected so the fixture cannot silently use IDs from a different dataset.
inline bool selectWaypointSet(const int waypointCount, WaypointSet* result)
{
	if (result == nullptr)
		return false;

	if (waypointCount == 95)
	{
		*result = WaypointSet{ 7, 10, 8 };
		return true;
	}

	if (waypointCount >= 157)
	{
		*result = WaypointSet{ 147, 149, 156 };
		return true;
	}

	return false;
}
}

#endif // _BOT_EMPIRES_COMBAT_FIXTURE_WAYPOINTS_H_
