// Offline unit tests for bot_empires_skirmish_logic.h (TODO 3.25).
#include "bot_empires_skirmish_logic.h"

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

bool Near(float a, float b, float eps = 0.01f)
{
	return std::fabs(a - b) <= eps;
}
} // namespace

int main()
{
	using namespace EmpiresSkirmishLogic;

	// 1. Shotgun closes in aggressively.
	{
		SkirmishBandInput in;
		in.weaponMaxRange = 700.0f;
		in.classId = kClassScout;
		const SkirmishBandResult r = ComputeSkirmishBand(in);
		Expect(Near(r.nearDist, 110.0f) && Near(r.farDist, 380.0f), "shotgun band 110..380");
		Expect(Near(r.crouchChance, 0.10f), "shotgun low crouch chance");
		Expect(!r.rocketStandoff, "shotgun not rocket standoff");
	}

	// 2. SMG / pistol holds mid-close range.
	{
		SkirmishBandInput in;
		in.weaponMaxRange = 1600.0f;
		in.classId = kClassEngineer;
		const SkirmishBandResult r = ComputeSkirmishBand(in);
		Expect(Near(r.nearDist, 180.0f) && Near(r.farDist, 650.0f), "SMG band 180..650");
		Expect(Near(r.crouchChance, 0.20f), "SMG crouch chance 0.20");
	}

	// 3. Rifle / 50cal / HMG keeps mid-range discipline.
	{
		SkirmishBandInput in;
		in.weaponMaxRange = 2800.0f;
		in.classId = kClassRifleman;
		const SkirmishBandResult r = ComputeSkirmishBand(in);
		Expect(Near(r.nearDist, 300.0f) && Near(r.farDist, 1050.0f), "rifle/HMG band 300..1050");
		Expect(Near(r.crouchChance, 0.30f), "rifle/HMG crouch chance 0.30");
	}

	// 4. Scout Sniper keeps standoff range (both via WEAP_FL_SCOPE and minRange >= 350).
	{
		SkirmishBandInput in;
		in.weaponMaxRange = 4500.0f;
		in.weaponMinRange = 350.0f;
		in.weaponScoped = true;
		in.classId = kClassScout;
		const SkirmishBandResult r = ComputeSkirmishBand(in);
		Expect(Near(r.nearDist, 550.0f) && Near(r.farDist, 1600.0f), "sniper band 550..1600");
		Expect(Near(r.crouchChance, 0.35f), "sniper crouch chance 0.35");

		// Even without explicit scope flag, minRange >= 350 selects sniper band.
		in.weaponScoped = false;
		const SkirmishBandResult r2 = ComputeSkirmishBand(in);
		Expect(Near(r2.nearDist, 550.0f) && Near(r2.farDist, 1600.0f), "minRange>=350 selects sniper band");
	}

	// 5. Grenadier RPG vs vehicle keeps 550..1300 standoff; vs infantry caps far at 880 and near >= 400.
	{
		SkirmishBandInput in;
		in.weaponMaxRange = 3200.0f;
		in.weaponMinRange = 300.0f;
		in.classId = kClassGrenadier;
		in.enemyInVehicle = true;
		in.grenadierRpgEnabled = true;
		in.hasRocket = true;
		const SkirmishBandResult rv = ComputeSkirmishBand(in);
		Expect(rv.rocketStandoff && Near(rv.nearDist, 550.0f) && Near(rv.farDist, 1300.0f),
			"grenadier RPG vs vehicle 550..1300");

		in.enemyInVehicle = false;
		const SkirmishBandResult ri = ComputeSkirmishBand(in);
		Expect(!ri.rocketStandoff && Near(ri.nearDist, 400.0f) && Near(ri.farDist, 880.0f),
			"grenadier RPG vs soldier 400..880");

		// Grenadier out of rockets falls back to pistol/sidearm near distance, still caps far <= 880.
		in.hasRocket = false;
		in.weaponMaxRange = 1400.0f;
		in.weaponMinRange = 0.0f;
		const SkirmishBandResult rp = ComputeSkirmishBand(in);
		Expect(Near(rp.nearDist, 180.0f) && Near(rp.farDist, 650.0f),
			"grenadier sidearm without rockets uses SMG/pistol band");
	}

	// 6. Unknown / zero weaponMaxRange falls back cleanly to SMG-class band (1200 HU).
	{
		SkirmishBandInput in;
		const SkirmishBandResult r = ComputeSkirmishBand(in);
		Expect(Near(r.nearDist, 180.0f) && Near(r.farDist, 650.0f), "zero maxRange fallback 180..650");
	}

	// 7. Leave-distance hysteresis and cap.
	Expect(Near(ComputeLeaveDist(1050.0f, false, false, false), 1050.0f), "entry leaveDist == farDist");
	Expect(Near(ComputeLeaveDist(1050.0f, true, false, false), 1312.5f), "active hysteresis 1.25x");
	Expect(Near(ComputeLeaveDist(1600.0f, true, false, false), 2000.0f), "sniper active leaveDist capped at 2000");
	Expect(Near(ComputeLeaveDist(1800.0f, true, false, false), 2000.0f), "over-cap leaveDist clamped to 2000");
	Expect(Near(ComputeLeaveDist(650.0f, false, true, false), 1200.0f), "scoutFlank floor 1200");
	Expect(Near(ComputeLeaveDist(650.0f, false, false, true), 1600.0f), "engineerRear floor 1600");

	// 8. Crouch cadence.
	Expect(ShouldCrouchPause(true, false, 200.0f, 0, 0.1f, 0.99f), "stage1Blocked always crouches");
	Expect(!ShouldCrouchPause(false, true, 450.0f, 1, 0.35f, 0.01f), "never crouch-pause below 500 HU");
	Expect(!ShouldCrouchPause(false, true, 600.0f, 1, 0.35f, 0.25f), "500..700 HU caps crouch chance at 0.20");
	Expect(ShouldCrouchPause(false, true, 600.0f, 1, 0.35f, 0.15f), "500..700 HU pauses below 0.20");
	Expect(ShouldCrouchPause(false, true, 900.0f, 1, 0.35f, 0.30f), ">700 HU uses full sniper crouch chance 0.35");
	Expect(!ShouldCrouchPause(false, false, 900.0f, 1, 0.35f, 0.05f), "non-strafe mode does not crouch-pause");
	Expect(!ShouldCrouchPause(false, true, 900.0f, 2, 0.35f, 0.05f), "only phase 1 transitions into crouch-pause");

	// 9. Hurt-cover action selection (no legacy CGotoHideSpotSched when navmesh is loaded).
	Expect(DecideHurtCoverAction(true, true, false, true, false) == HURT_COVER_NONE,
		"below damage threshold -> no cover");
	Expect(DecideHurtCoverAction(true, true, true, true, true) == HURT_COVER_NONE,
		"active skirmish with fight_cover owns cover");
	Expect(DecideHurtCoverAction(true, true, false, true, true) == HURT_COVER_USE_NAVMESH,
		"outside skirmish with navmesh -> navmesh hurt-cover");
	Expect(DecideHurtCoverAction(true, true, true, false, true) == HURT_COVER_USE_NAVMESH,
		"skirmish with fight_cover=0 still uses navmesh hurt-cover");
	Expect(DecideHurtCoverAction(false, true, false, true, true) == HURT_COVER_USE_LEGACY_WAYPOINT,
		"cvar off -> legacy fallback");
	Expect(DecideHurtCoverAction(true, false, false, true, true) == HURT_COVER_USE_LEGACY_WAYPOINT,
		"no navmesh loaded -> legacy fallback");

	// 10. Hurt-cover timeout and priority preemption.
	Expect(!ShouldEndHurtCover(2.0f, false, 0.0f, 2.5f, false), "cover continues while moving under cap");
	Expect(!ShouldEndHurtCover(3.0f, true, 1.5f, 2.5f, false), "cover waits until waitDuration elapses");
	Expect(ShouldEndHurtCover(4.0f, true, 2.5f, 2.5f, false), "cover ends after route + wait");
	Expect(ShouldEndHurtCover(6.0f, false, 0.0f, 3.0f, false), "cover hard-capped at 6 s");
	Expect(ShouldEndHurtCover(0.5f, false, 0.0f, 3.0f, true), "higher-priority job ends cover immediately");

	// 11. Cover fire-permission (no cover=holding-fire against visible enemy unless reloading).
	Expect(!ShouldHoldFireInCover(false, true, false, true), "no cover -> never hold fire");
	Expect(ShouldHoldFireInCover(true, false, true, false), "legacy cvar off -> holds fire");
	Expect(!ShouldHoldFireInCover(true, true, true, false), "navmesh cover + visible enemy + loaded -> keep shooting");
	Expect(ShouldHoldFireInCover(true, true, true, true), "navmesh cover + reloading -> hold fire");
	Expect(ShouldHoldFireInCover(true, true, false, false), "navmesh cover + enemy not visible -> hold fire");

	// 3.25b: Empires hurt trigger, 35 % of max health within 2 s, then 9 s cooldown.
	{
		HurtWindow w;
		Expect(!AddHurtDamage(w, 10.0f, 20.0f, 100.0f), "20 HP: no cover yet");
		Expect(AddHurtDamage(w, 11.5f, 20.0f, 100.0f), "40 HP within 2 s: cover");
		Expect(!AddHurtDamage(w, 12.0f, 60.0f, 100.0f), "cooldown: heavy fire right after does not re-trigger");
		Expect(!AddHurtDamage(w, 20.0f, 10.0f, 100.0f), "still in cooldown at 20 s");
		Expect(AddHurtDamage(w, 21.0f, 40.0f, 100.0f), "after the cooldown 40 HP triggers again");
		HurtWindow slow;
		Expect(!AddHurtDamage(slow, 0.0f, 20.0f, 100.0f) && !AddHurtDamage(slow, 2.5f, 20.0f, 100.0f),
			"20 + 20 HP spread over 2.5 s does not trigger (window restarts)");
		HurtWindow none;
		Expect(!AddHurtDamage(none, 0.0f, 50.0f, 0.0f), "unknown max health never triggers");
	}

	// 3.25b: health polling replaces the missing player_hurt event.
	{
		PolledHurtInput in;
		in.lastHealth = 100;
		in.health = 80;
		in.enemySeenAgo = 0.2f;
		PolledHurtDecision d = DecidePolledHurt(in);
		Expect(d.fire && d.damage == 20 && d.passAttacker && d.allowCover && d.interruptEngineer,
			"20 HP with a visible enemy: hurt with attacker, cover allowed, engineer may stop");
		in.enemySeenAgo = 5.0f;
		d = DecidePolledHurt(in);
		Expect(d.fire && !d.passAttacker && d.interruptEngineer, "enemy seen 5 s ago: no attacker, 20 % still interrupts");
		in.health = 90;
		d = DecidePolledHurt(in);
		Expect(d.fire && !d.passAttacker && !d.interruptEngineer, "unseen 10 HP graze: hurt, engineer keeps working");
		in.enemySeenAgo = -1.0f;
		in.health = 98;
		Expect(!DecidePolledHurt(in).fire, "2 HP: filtered");
		in.health = 70;
		in.worldDamage = true;
		Expect(!DecidePolledHurt(in).fire, "world damage (fall): filtered");
		in.worldDamage = false;
		in.lastHealth = 0;
		Expect(!DecidePolledHurt(in).fire, "no reading from this life: nothing");
		in.lastHealth = 100;
		in.inVehicle = true;
		Expect(!DecidePolledHurt(in).fire, "in a vehicle: nothing");
		in.inVehicle = false;
		in.alive = false;
		Expect(!DecidePolledHurt(in).fire, "dead: nothing");
		in.alive = true;
		in.enabled = false;
		Expect(!DecidePolledHurt(in).fire, "cvar off: nothing");
		in.enabled = true;
		in.health = 120;
		Expect(!DecidePolledHurt(in).fire, "healed: nothing");
		in.health = 50;
		in.buildAssigned = true;
		in.enemySeenAgo = 0.0f;
		d = DecidePolledHurt(in);
		Expect(d.fire && d.passAttacker && !d.allowCover && !d.interruptEngineer,
			"build assignment: hurt bookkeeping only, no cover, no engineer stop");
	}

	if (g_failures != 0)
	{
		std::fprintf(stderr, "%d failure(s)\n", g_failures);
		return 1;
	}
	std::printf("skirmish logic tests PASS\n");
	return 0;
}
