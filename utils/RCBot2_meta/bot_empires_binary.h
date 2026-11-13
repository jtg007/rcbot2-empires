#pragma once

// Version check of the game binary (empires/bin/server_srv.so). Several
// features read or write fixed offsets in game objects that no SendProp or
// datamap describes. A game update would silently make them wrong, so those
// places only run when the binary matches a known good build (or
// rcbot_empires_force_offsets 1). The check runs once and is cached.
bool EmpiresBinaryOffsetsTrusted();
