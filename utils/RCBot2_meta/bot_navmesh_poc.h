/*
 * Isolated NavMesh spike for Empires.
 *
 * The production API deliberately stops at path construction. The separate
 * test-only runner may consume those points, but it does not replace
 * RCBot2's waypoint navigator.
 */
#ifndef __RCBOT_NAVMESH_POC_H__
#define __RCBOT_NAVMESH_POC_H__

#include "bot_navmesh_api.h"

namespace RCBotNavMeshPoc
{
void Frame();
void RecordHumanMovementFrame();
void LevelShutdown();
void ReconcileBuildings();
void ResetBuildings();
bool BuildBuildingAwarePath(const Vector &from, const Vector &to,
	RCBotNavMesh::Path *path);
}

#endif
