// Offline unit tests for bot_empires_team_intel.h (TODO 3.20 / Audit C9).
#include "bot_empires_team_intel.h"

#include <cmath>
#include <cstdio>

namespace
{
int g_failures = 0;

void Expect(bool cond, const char* msg)
{
	if (!cond)
	{
		std::fprintf(stderr, "FAIL: %s\n", msg);
		++g_failures;
	}
}

bool Near(float a, float b, float eps = 0.1f)
{
	return std::fabs(a - b) <= eps;
}
} // namespace

int main()
{
	using namespace EmpiresTeamIntel;

	// 1. Cvar mode filtering (A/B testing support).
	Expect(!FairIntelEnabledForTeam(0, 2) && !FairIntelEnabledForTeam(0, 3), "cvar 0 disables both teams");
	Expect(FairIntelEnabledForTeam(1, 2) && FairIntelEnabledForTeam(1, 3), "cvar 1 enables both teams");
	Expect(FairIntelEnabledForTeam(2, 2) && !FairIntelEnabledForTeam(2, 3), "cvar 2 enables NF only");
	Expect(!FairIntelEnabledForTeam(3, 2) && FairIntelEnabledForTeam(3, 3), "cvar 3 enables BE only");

	// 2. TTL expiry per contact kind: infantry 10 s, vehicle/APC 20 s, building 180 s, CV 25 s.
	Board b;
	const float pInf[3] = { 100.0f, 0.0f, 0.0f };
	const float pVeh[3] = { 200.0f, 0.0f, 0.0f };
	const float pApc[3] = { 300.0f, 0.0f, 0.0f };
	const float pBld[3] = { 400.0f, 0.0f, 0.0f };
	const float pCv0[3] = { 5000.0f, 5000.0f, 0.0f };
	const float pCvLive[3] = { 4200.0f, 4800.0f, 0.0f };

	SetInitialEnemyCv(b, pCv0);
	Upsert(b, 5, 101, CONTACT_INFANTRY, SOURCE_SIGHT, pInf, 100.0f);
	Upsert(b, 50, 201, CONTACT_VEHICLE, SOURCE_SIGHT, pVeh, 100.0f);
	Upsert(b, 51, 202, CONTACT_APC, SOURCE_SPOTTED, pApc, 100.0f);
	Upsert(b, 60, 301, CONTACT_BUILDING, SOURCE_CAMERA, pBld, 100.0f);
	Upsert(b, 70, 401, CONTACT_ENEMY_CV, SOURCE_SIGHT, pCvLive, 100.0f);

	Expect(IsKnown(b, 5, 101, 109.9f), "infantry known within 10 s TTL");
	Expect(!IsKnown(b, 5, 101, 110.1f), "infantry expires after 10 s TTL");
	Expect(!IsKnown(b, 5, 999, 105.0f), "recycled entity serial is rejected");

	Expect(IsKnown(b, 50, 201, 119.9f) && !IsKnown(b, 50, 201, 120.1f), "vehicle expires at 20 s TTL");
	Expect(IsKnown(b, 51, 202, 119.9f) && !IsKnown(b, 51, 202, 120.1f), "APC expires at 20 s TTL");
	Expect(IsKnown(b, 70, 401, 124.9f) && !IsKnown(b, 70, 401, 125.1f), "enemy CV live sight expires at 25 s TTL");
	Expect(IsKnown(b, 60, 301, 279.9f) && !IsKnown(b, 60, 301, 280.1f), "building persists for 180 s TTL");

	// 3. EnemyBaseAnchor fallback chain: live CV -> last known CV -> initial base spawn.
	{
		float anchor[3] = {};
		bool live = false;
		Expect(EnemyBaseAnchor(b, 110.0f, anchor, live) && live && Near(anchor[0], 4200.0f),
			"live CV contact takes priority within 25 s");
		Expect(EnemyBaseAnchor(b, 130.0f, anchor, live) && !live && Near(anchor[0], 4200.0f),
			"after 25 s falls back to last confirmed CV position with isLiveSight=false");

		Board fresh;
		SetInitialEnemyCv(fresh, pCv0);
		// Second SetInitialEnemyCv must NOT overwrite the start-of-round anchor.
		SetInitialEnemyCv(fresh, pCvLive);
		Expect(EnemyBaseAnchor(fresh, 10.0f, anchor, live) && !live && Near(anchor[0], 5000.0f),
			"unseen CV uses immutable initial main-base spawn anchor");
	}

	// 4. Sensor ranges & source classification.
	{
		const Sensor cam{ { 0.0f, 0.0f, 0.0f }, false };
		const Sensor rad{ { 0.0f, 0.0f, 0.0f }, true };
		const float at1100[3] = { 1100.0f, 0.0f, 0.0f };
		const float at1500[3] = { 1500.0f, 0.0f, 0.0f };
		const float at2000[3] = { 2000.0f, 0.0f, 0.0f };
		Expect(SensorInRange(cam, at1100) && !SensorInRange(cam, at1500), "camera range is 1200 HU");
		Expect(SensorInRange(rad, at1500) && !SensorInRange(rad, at2000), "radar range is 1800 HU");
		Expect(ClassifyFairSource(false, false, false, false) == SOURCE_NONE, "no source -> SOURCE_NONE");
		Expect(ClassifyFairSource(false, false, true, false) == SOURCE_RADAR, "radar-only -> SOURCE_RADAR");
		Expect(ClassifyFairSource(false, false, true, true) == SOURCE_CAMERA, "camera beats radar");
		Expect(ClassifyFairSource(false, true, true, true) == SOURCE_SPOTTED, "game spotted beats sensors");
		Expect(ClassifyFairSource(true, true, true, true) == SOURCE_SIGHT, "direct sight is top source");
	}

	// 5. Capacity cap (32 slots): high-priority APC/CV never evicted by infantry spam.
	{
		Board full;
		Upsert(full, 900, 1, CONTACT_ENEMY_CV, SOURCE_SIGHT, pCvLive, 50.0f);
		Upsert(full, 901, 2, CONTACT_APC, SOURCE_SIGHT, pApc, 50.0f);
		for (int i = 1; i <= 40; ++i)
		{
			const float p[3] = { static_cast<float>(i * 10), 0.0f, 0.0f };
			Upsert(full, i, i, CONTACT_INFANTRY, SOURCE_SIGHT, p, 50.0f + i * 0.1f);
		}
		Expect(CountActive(full, 0u, 55.0f) == kMaxContacts, "board holds at most 32 contacts");
		Expect(IsKnown(full, 900, 1, 55.0f), "enemy CV survives 40 infantry inserts");
		Expect(IsKnown(full, 901, 2, 55.0f), "enemy APC survives 40 infantry inserts");
		Expect(IsKnown(full, 40, 40, 55.0f), "newest infantry contact stored");
		Expect(!IsKnown(full, 1, 1, 55.0f), "oldest infantry contact evicted");

		// A new vehicle evicts an infantry slot when full.
		Expect(Upsert(full, 902, 3, CONTACT_VEHICLE, SOURCE_SIGHT, pVeh, 56.0f),
			"vehicle evicts lower-priority infantry slot");
		Expect(IsKnown(full, 902, 3, 56.0f), "inserted vehicle is active");
	}

	// 6. Own damage events (fair base alert without leaking attacker coords).
	{
		Board dBoard;
		const float refPos[3] = { 1000.0f, 2000.0f, 0.0f };
		const float cvPos[3] = { 0.0f, 0.0f, 0.0f };
		RecordOwnDamage(dBoard, 136, refPos, false, 200.0f);
		Expect(HasRecentOwnDamage(dBoard, 136, 210.0f), "refinery damage active for 15 s");
		Expect(!HasRecentOwnDamage(dBoard, 136, 216.0f), "refinery damage expires after 15 s");

		RecordOwnDamage(dBoard, 136, refPos, false, 220.0f);
		RecordOwnDamage(dBoard, 99, cvPos, true, 219.0f);
		DamageEvent top;
		Expect(LatestOwnDamage(dBoard, 221.0f, top) && top.isOwnCv && top.ownEntity == 99,
			"own CV damage takes priority over refinery damage");
	}

	// 7. Vehicle strategic goal selection (Defend / Damage / Hunt / Push / Base Anchor).
	{
		Board vBoard;
		const float ownCv[3] = { 0.0f, 0.0f, 0.0f };
		const float ownAssets[2][3] = { { 0.0f, 0.0f, 0.0f }, { 2000.0f, 0.0f, 0.0f } };
		const float tankPos[3] = { 1500.0f, 0.0f, 0.0f };
		const float enemyBase[3] = { 10000.0f, 0.0f, 0.0f };
		SetInitialEnemyCv(vBoard, enemyBase);

		// No contacts yet -> falls back to EnemyBaseAnchor (keeps R5 vehicle_front_max green).
		const VehicleIntelGoal gBase = SelectVehicleStrategicGoal(vBoard, tankPos, ownAssets, 2, ownCv, 10.0f);
		Expect(gBase.kind == VehicleIntelGoal::GOAL_BASE_ANCHOR && Near(gBase.pos[0], 10000.0f),
			"no spotted enemies -> tank pushes toward enemy base anchor");

		// Spotted enemy forward building closer than base - 1500 -> GOAL_PUSH_BUILDING.
		const float fwdRef[3] = { 6000.0f, 0.0f, 0.0f };
		Upsert(vBoard, 300, 1, CONTACT_BUILDING, SOURCE_SIGHT, fwdRef, 10.0f);
		const VehicleIntelGoal gPush = SelectVehicleStrategicGoal(vBoard, tankPos, ownAssets, 2, ownCv, 11.0f);
		Expect(gPush.kind == VehicleIntelGoal::GOAL_PUSH_BUILDING && gPush.entity == 300,
			"known forward enemy building beats base anchor");

		// Spotted enemy vehicle within 6000 HU (outside own asset defend radius) -> GOAL_HUNT_VEHICLE.
		const float enemyTankFar[3] = { 6200.0f, 0.0f, 0.0f };
		Upsert(vBoard, 301, 1, CONTACT_VEHICLE, SOURCE_SIGHT, enemyTankFar, 12.0f);
		const VehicleIntelGoal gHunt = SelectVehicleStrategicGoal(vBoard, tankPos, ownAssets, 2, ownCv, 12.0f);
		Expect(gHunt.kind == VehicleIntelGoal::GOAL_HUNT_VEHICLE && gHunt.entity == 301,
			"spotted enemy tank in hunt range beats building push");

		// Own refinery takes damage from an unseen attacker -> GOAL_DEFEND_DAMAGE beats hunt!
		RecordOwnDamage(vBoard, 136, ownAssets[1], false, 13.0f);
		const VehicleIntelGoal gDmg = SelectVehicleStrategicGoal(vBoard, tankPos, ownAssets, 2, ownCv, 13.0f);
		Expect(gDmg.kind == VehicleIntelGoal::GOAL_DEFEND_DAMAGE && gDmg.entity == 136,
			"fair own-building damage alert overrides distant hunt");

		// Spotted enemy APC near own refinery (within 3500) -> GOAL_DEFEND_VEHICLE (APC).
		const float enemyApc[3] = { 2400.0f, 0.0f, 0.0f };
		Upsert(vBoard, 302, 1, CONTACT_APC, SOURCE_SPOTTED, enemyApc, 14.0f);
		const VehicleIntelGoal gApc = SelectVehicleStrategicGoal(vBoard, tankPos, ownAssets, 2, ownCv, 14.0f);
		Expect(gApc.kind == VehicleIntelGoal::GOAL_DEFEND_VEHICLE && gApc.entity == 302 && gApc.isApc,
			"spotted APC near own asset triggers defend");

		// Immediate tank threat < 1500 HU from own CV overrides APC priority (TODO 3.24).
		const float cvThreatTank[3] = { 900.0f, 0.0f, 0.0f };
		Upsert(vBoard, 303, 1, CONTACT_VEHICLE, SOURCE_SIGHT, cvThreatTank, 15.0f);
		const VehicleIntelGoal gCv = SelectVehicleStrategicGoal(vBoard, tankPos, ownAssets, 2, ownCv, 15.0f);
		Expect(gCv.kind == VehicleIntelGoal::GOAL_DEFEND_VEHICLE && gCv.entity == 303 && !gCv.isApc,
			"immediate enemy tank < 1500 HU from own CV overrides APC priority");
	}

	// 8. APC cover-park anchor, Lawliet resolution, and Commander CV retreat threat.
	{
		Board aBoard;
		const float ownSide[3] = { 0.0f, 0.0f, 0.0f };
		const float enemyBase[3] = { 9000.0f, 0.0f, 0.0f };
		const float midRes[2][3] = { { 4500.0f, 200.0f, 0.0f }, { 6000.0f, -100.0f, 0.0f } };
		SetInitialEnemyCv(aBoard, enemyBase);

		float anchor[3] = {};
		bool fromBldg = false;
		Expect(SelectApcCoverAnchor(aBoard, ownSide, midRes, 2, 10.0f, anchor, fromBldg) &&
			!fromBldg && Near(anchor[0], 4500.0f),
			"APC cover anchor uses nearest mid resource when no enemy building is spotted");

		Expect(SelectApcCoverAnchor(aBoard, ownSide, nullptr, 0, 10.0f, anchor, fromBldg) &&
			!fromBldg && Near(anchor[0], 9000.0f),
			"APC cover anchor falls back to EnemyBaseAnchor without resources");

		const float knownBldg[3] = { 5200.0f, 0.0f, 0.0f };
		Upsert(aBoard, 400, 1, CONTACT_BUILDING, SOURCE_SIGHT, knownBldg, 10.0f);
		Expect(SelectApcCoverAnchor(aBoard, ownSide, midRes, 2, 11.0f, anchor, fromBldg) &&
			fromBldg && Near(anchor[0], 5200.0f),
			"APC cover anchor prefers spotted enemy building over resource/base");

		// Lawliet starts on base anchor and upgrades to live CV once spotted.
		bool live = false;
		Expect(ResolveLawlietCvTarget(aBoard, 11.0f, anchor, live) && !live && Near(anchor[0], 9000.0f),
			"Lawliet targets initial enemy base before CV is sighted");
		const float movedCv[3] = { 8200.0f, 1500.0f, 0.0f };
		Upsert(aBoard, 401, 1, CONTACT_ENEMY_CV, SOURCE_SIGHT, movedCv, 12.0f);
		Expect(ResolveLawlietCvTarget(aBoard, 12.0f, anchor, live) && live && Near(anchor[0], 8200.0f),
			"Lawliet upgrades to live enemy CV once sighted");

		// Commander CV retreat threat: unseen -> false; known vehicle within 2200 -> true.
		float threatPos[3] = {};
		Expect(!ResolveCvRetreatThreat(aBoard, ownSide, 2200.0f, 12.0f, threatPos),
			"CV retreat does not cheat when no enemy vehicle is known nearby");
		const float nearTank[3] = { 1800.0f, 0.0f, 0.0f };
		Upsert(aBoard, 402, 1, CONTACT_VEHICLE, SOURCE_RADAR, nearTank, 12.0f);
		Expect(ResolveCvRetreatThreat(aBoard, ownSide, 2200.0f, 12.0f, threatPos) && Near(threatPos[0], 1800.0f),
			"CV retreat detects radar-spotted enemy vehicle within 2200 HU");
	}

	// 9. Remove, Prune & Level Reset.
	{
		Upsert(b, 77, 1, CONTACT_INFANTRY, SOURCE_SIGHT, pInf, 100.0f);
		Expect(IsKnown(b, 77, 1, 105.0f), "contact 77 inserted before Remove");
		Remove(b, 77);
		Expect(!IsKnown(b, 77, 1, 105.0f), "Remove clears contact immediately");

		Prune(b, 500.0f);
		Expect(CountActive(b, 0u, 500.0f) == 0, "Prune clears all expired contacts");
		b.Reset();
		float anchor[3] = {};
		bool live = false;
		DamageEvent d;
		Expect(!EnemyBaseAnchor(b, 0.0f, anchor, live), "Reset clears initial and last-known enemy CV");
		Expect(!LatestOwnDamage(b, 0.0f, d), "Reset clears damage events");
	}
	{
		Board r;
		const float p[3] = { 10.0f, 20.0f, 30.0f };
		SetInitialEnemyCv(r, p);
		RecordOwnDamage(r, 10, p, true, 5.0f);
		Upsert(r, 20, 1, CONTACT_INFANTRY, SOURCE_SIGHT, p, 5.0f);
		r.Reset();
		DamageEvent d;
		Expect(!LatestOwnDamage(r, 5.0f, d) && CountActive(r, 0u, 5.0f) == 0 && !r.initialEnemyCvKnown,
			"Board::Reset clears contacts, damage events, and base anchors completely");
	}

	if (g_failures != 0)
	{
		std::fprintf(stderr, "%d failure(s)\n", g_failures);
		return 1;
	}
	std::printf("team intel tests PASS\n");
	return 0;
}
