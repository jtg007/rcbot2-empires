#include "bot_empires_squad_join.h"

#include <cassert>

using namespace RCBotEmpiresSquadJoin;

int main()
{
	Input input;
	input.unassignedBots = 2;
	input.currentUserId = input.lowestUnassignedUserId = 10;
	assert(Select(input).squadId == 1); // SQJOIN-01

	input.squads = { { 1, 4, false } };
	assert(Select(input).squadId == 1); // SQJOIN-02
	input.squads = { { 1, 5, false } };
	assert(Select(input).squadId == 2); // SQJOIN-03
	input.squads.push_back({ 2, 5, false });
	assert(Select(input).squadId == 3); // SQJOIN-04
	input.squads = { { 2, 5, false }, { 1, 3, false } };
	assert(Select(input).squadId == 1); // SQJOIN-05, deterministic order
	input.squads = { { 1, 5, false } };
	assert(Select(input).squadId != 1); // SQJOIN-06
	input.squads = { { 1, 5, false }, { 2, 4, false } };
	assert(Select(input).squadId == 2); // SQJOIN-07 refreshed state
	input.squads = { { 1, 5, false }, { 2, 5, false } };
	assert(Select(input).squadId == 3); // SQJOIN-08 isolated input snapshot
	input.squads = { { 4, 4, false }, { 2, 5, false } };
	assert(Select(input).squadId == 4); // SQJOIN-09 stable ordering
	input.ownSquad = 2;
	assert(Select(input).status == Status::ALREADY_ASSIGNED); // SQJOIN-10
	input.ownSquad = 0;
	input.attempts = kMaximumJoinAttempts;
	assert(Select(input).status == Status::NO_CANDIDATE); // SQJOIN-11
	assert(input.squads.size() == 2 && input.squads[0].id == 4); // SQJOIN-12

	Input wait;
	wait.unassignedBots = 1;
	assert(Select(wait).status == Status::WAIT_FOR_PEER);
	Input human;
	human.squads = { { 1, 5, true } };
	assert(Select(human).squadId == 2);
	return 0;
}
