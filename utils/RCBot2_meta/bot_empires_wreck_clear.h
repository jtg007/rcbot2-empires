// Pure-C++ decision helpers for clearing blocked vehicle factory bays and
// deconstructing vehicle carcasses (TODO 5.8a / 3.17). SDK-free for offline unit testing.
#ifndef __BOT_EMPIRES_WRECK_CLEAR_H__
#define __BOT_EMPIRES_WRECK_CLEAR_H__

namespace EmpiresWreckClear
{
constexpr float kFactoryBayRadius = 500.0f;    // emp_vehicle_build +-150 box + vehicle half-length
constexpr float kBayClearMaxWalk = 4500.0f;    // base engineers cross base to clear a blocked VF
constexpr float kBaseCarcassRadius = 2000.0f;  // carcasses near own CV / base exits
constexpr float kLocalCarcassRadius = 900.0f;  // carcasses right next to an engineer
constexpr float kBayPriorityBonus = -10000.0f; // factory bay blockers always outrank loose wrecks

struct CarcassEval
{
	bool eligible = false;
	bool blocksFactory = false;
	float score = 1.0e9f;
};

// Evaluates whether a destroyed vehicle carcass (alive == false, carcassHealth > 0)
// should be deconstructed by a free Engineer using the kit PrimaryAttack
// (CWeaponNFEngineerKit::PrimaryAttack 0x3d0d61..0x3d1096 removes the carcass and
// refunds team resources regardless of the carcass's original team).
inline CarcassEval EvaluateCarcass(bool alive, int carcassHealth, float distToEngineer,
	bool hasOwnFactory, float distToFactoryBuild, bool hasOwnCV, float distToCV)
{
	CarcassEval out;
	if (alive || carcassHealth <= 0)
		return out;
	const bool blocksFactory = hasOwnFactory && distToFactoryBuild <= kFactoryBayRadius;
	if (blocksFactory)
	{
		if (distToEngineer > kBayClearMaxWalk)
			return out;
		out.eligible = true;
		out.blocksFactory = true;
		out.score = distToEngineer + kBayPriorityBonus;
		return out;
	}
	const bool nearBase = hasOwnCV && distToCV <= kBaseCarcassRadius && distToEngineer <= kBaseCarcassRadius;
	const bool local = distToEngineer <= kLocalCarcassRadius;
	if (!nearBase && !local)
		return out;
	out.eligible = true;
	out.blocksFactory = false;
	out.score = distToEngineer;
	return out;
}

// Live enemy vehicle parked in/at our own vehicle factory bay: a free Grenadier
// within kBayClearMaxWalk should move to engage it so vehicle purchases resume.
inline bool IsEnemyFactoryBlocker(bool alive, int vehicleTeam, int ownTeam,
	bool hasOwnFactory, float distToFactoryBuild, float distToGrenadier)
{
	if (!alive || !hasOwnFactory)
		return false;
	if (ownTeam != 2 && ownTeam != 3)
		return false;
	const int enemyTeam = ownTeam == 2 ? 3 : 2;
	return vehicleTeam == enemyTeam &&
		distToFactoryBuild <= kFactoryBayRadius &&
		distToGrenadier <= kBayClearMaxWalk;
}
} // namespace EmpiresWreckClear

#endif

