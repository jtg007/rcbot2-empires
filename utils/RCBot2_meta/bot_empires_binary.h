#pragma once

// Version check of the game binary (empires/bin/server_srv.so). Several
// features read or write fixed offsets in game objects that no SendProp or
// datamap describes. A game update would silently make them wrong, so those
// places only run when the binary matches a known good build (or
// rcbot_empires_force_offsets 1). The check runs once and is cached.
bool EmpiresBinaryOffsetsTrusted();

// CEmpParams::RetrievePlayerRoundStats (0x645670), called from CEmpPlayer::InitialSpawn, restores the
// stored round record whose network ID matches the joining player and calls ChangeTeam(stored team).
// Every fake client's network ID is "BOT", so each new bot inherited the previous bot's record:
// Team -> Unassigned -> Team on join and two tickets instead of one. Renames the stored "BOT" records
// so a new bot starts clean; humans' records are untouched. Returns the number renamed, -1 if skipped.
int EmpiresForgetBotRoundStats();
