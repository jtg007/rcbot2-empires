#ifndef __RCBOT_NAVMESH_PROVIDER_H__
#define __RCBOT_NAVMESH_PROVIDER_H__

#include <cstdint>

#include "vector.h"

class CBot;

namespace RCBotNavMeshProvider
{
// Phase-4 alternative provider. It consumes the existing native Source
// NavMesh path API and writes only through the provider-neutral navigation
// ownership seam on CBot.
bool Start(CBot* bot, const Vector& goal);
// Start the NavMesh leg of an already-created provider-neutral intent.  The
// intent id is deliberately supplied so AUTO fallback keeps the same intent
// while receiving a fresh provider-specific NavigationAttempt id.
bool StartForIntent(CBot* bot, std::uint64_t intentId, const Vector& goal);
bool Cancel(CBot* bot, const char* reason);
bool StopForIntent(CBot* bot, std::uint64_t intentId);
bool IsActive(const CBot* bot);

// Called from the normal Empires GameFrame after botThink(). The next command
// is then produced by the existing CBot::doMove()/RunPlayerMove path.
void Frame();
void LevelShutdown();

// A func_button whose OnPressed opens a func_door, directly (Toggle/Open) or through a
// filter_activator_team OnPass. as_escort: control1 -> door1 (BE base gate, any team),
// control2 -> control2_filter (team 3) -> door2.
struct DoorButton
{
	int button = -1;
	int door = -1;
	int team = 0; // 0 = any team may press
	Vector origin = Vector(0, 0, 0);
};
// Nearest button (2D, within maxDistance) the team may press whose door rests closed.
// The button may still be locked (as_escort control1 unlocks 60 s after BE spawns).
bool FindDoorButton(int team, const Vector& from, float maxDistance, DoorButton& out);
// Its door still rests closed.
bool DoorButtonPressable(const DoorButton& entry);
bool DoorButtonLocked(const DoorButton& entry);
}

#endif
