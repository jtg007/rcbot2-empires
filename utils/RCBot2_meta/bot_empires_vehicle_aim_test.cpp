// Offline unit tests for bot_empires_vehicle_aim.h (TODO 3.21 / 3.21a / 3.10f).
#include "bot_empires_vehicle_aim.h"

#include <cmath>
#include <cstdio>
#include <cstring>

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

bool Near(float a, float b, float eps = 0.05f)
{
	return std::fabs(a - b) <= eps;
}
} // namespace

int main()
{
	using namespace EmpiresVehicleAim;

	// 1. Angle normalization & wrap-around yaw error.
	Expect(Near(YawErrorDeg(179.0f, -179.0f), 2.0f), "wrap-around yaw error across +-180");
	Expect(Near(YawErrorDeg(-170.0f, 175.0f), 15.0f), "wrap-around yaw error 15 deg");
	Expect(HasRotatableTurret(true, -180.0f, 180.0f), "360 turret is rotatable");
	Expect(!HasRotatableTurret(true, 0.0f, 0.0f), "fixed weapon (min==max) is not rotatable");
	Expect(!HasRotatableTurret(false, -180.0f, 180.0f), "null turret is not rotatable");

	// 2. TurretAlignedToFire: MG (8 deg), cannon (12 deg), settle time, fallback timeout, turret-free.
	{
		TurretAlignInput in;
		in.turretYawDeg = 90.0f;
		in.wantYawDeg = 96.0f; // 6 deg off
		in.infantryTarget = true;
		in.aimedForSeconds = 0.2f;
		Expect(!TurretAlignedToFire(in).canFire, "blocked during initial 0.4 s settle");

		in.aimedForSeconds = 0.5f;
		const TurretAlignResult rMgOk = TurretAlignedToFire(in);
		Expect(rMgOk.canFire && !rMgOk.fallbackEscalated, "MG fires at 6 deg error after 0.5 s");

		in.wantYawDeg = 100.0f; // 10 deg off: too wide for MG, OK for cannon
		Expect(!TurretAlignedToFire(in).canFire, "MG holds fire at 10 deg yaw error");

		in.infantryTarget = false;
		Expect(TurretAlignedToFire(in).canFire, "cannon fires at 10 deg yaw error (<= 12 deg)");

		in.wantYawDeg = 120.0f; // 30 deg off
		Expect(!TurretAlignedToFire(in).canFire, "cannon holds fire at 30 deg yaw error");

		in.aimedForSeconds = 2.5f;
		const TurretAlignResult rEsc = TurretAlignedToFire(in);
		Expect(rEsc.canFire && rEsc.fallbackEscalated, "2.5 s timeout releases fire with escalation");

		// No rotatable turret -> legacy 1.2 s timer.
		in.hasRotatableTurret = false;
		in.aimedForSeconds = 1.0f;
		Expect(!TurretAlignedToFire(in).canFire, "non-rotatable turret waits for legacy 1.2 s");
		in.aimedForSeconds = 1.3f;
		const TurretAlignResult rLegacy = TurretAlignedToFire(in);
		Expect(rLegacy.canFire && !rLegacy.fallbackEscalated, "non-rotatable turret fires after 1.2 s without escalation");
	}

	// 2b. Distance-scaled MG alignment tolerance, chest aim height, front-to-rear pitch blend, and MG parallax.
	{
		Expect(Near(MgAlignToleranceDeg(0.0f), 8.0f), "zero range keeps default 8 deg MG tolerance");
		Expect(Near(MgAlignToleranceDeg(300.0f), 8.0f), "close range clamps at 8 deg MG tolerance");
		Expect(MgAlignToleranceDeg(600.0f) > 5.5f && MgAlignToleranceDeg(600.0f) < 5.9f, "600 HU MG tolerance ~5.7 deg");
		Expect(Near(MgAlignToleranceDeg(860.0f), 4.0f, 0.05f), "860 HU MG tolerance ~4.0 deg");
		Expect(Near(MgAlignToleranceDeg(1100.0f), 4.0f), "1100 HU MG tolerance clamps at 4.0 deg");

		TurretAlignInput in;
		in.turretYawDeg = 90.0f;
		in.wantYawDeg = 96.0f; // 6 deg off: OK close (300 HU), too wide at 850 HU
		in.infantryTarget = true;
		in.aimedForSeconds = 0.5f;
		in.targetRange = 300.0f;
		Expect(TurretAlignedToFire(in).canFire, "MG fires at 6 deg error when close (300 HU)");
		in.targetRange = 850.0f;
		Expect(!TurretAlignedToFire(in).canFire, "MG holds fire at 6 deg error at 850 HU");
		in.wantYawDeg = 93.0f; // 3 deg off: aligned at 850 HU (no false weave stall)
		Expect(TurretAlignedToFire(in).canFire, "MG fires at 3 deg error at 850 HU");

		Expect(Near(InfantryAimHeight(72.0f), 48.96f, 0.1f), "standing soldier chest height ~49 HU");
		Expect(Near(InfantryAimHeight(36.0f), 22.32f, 0.1f), "crouching soldier chest height ~22 HU");
		Expect(Near(InfantryAimHeight(16.0f), 8.0f, 0.1f), "prone soldier aim height ~8 HU");

		float minP = 0.0f, maxP = 0.0f;
		BlendedPitchLimits(90.0f, -30.0f, 10.0f, -30.0f, 3.0f, minP, maxP);
		Expect(Near(minP, -30.0f) && Near(maxP, 10.0f), "front localYaw=90 uses full front pitch limit");
		BlendedPitchLimits(-90.0f, -30.0f, 10.0f, -30.0f, 3.0f, minP, maxP);
		Expect(Near(minP, -30.0f) && Near(maxP, 3.0f), "rear localYaw=-90 uses rear pitch limit");
		BlendedPitchLimits(180.0f, -30.0f, 10.0f, -30.0f, 3.0f, minP, maxP);
		Expect(Near(minP, -30.0f) && Near(maxP, 6.5f), "side localYaw=180 blends halfway between front and rear");

		// MG parallax aim shift: matches measured NF Heavy X-ray (dist=768, behind=508 -> +11.95 HU right).
		float rShift = 0.0f, uShift = 0.0f;
		Expect(HasMgParallaxOffset(5, false) && !HasMgParallaxOffset(2, false) && !HasMgParallaxOffset(13, false),
			"parallax offset active for NF Heavy, inactive for coaxial NF Light / BE Heavy");
		MgParallaxAimShift(5, false, 768.0f, 508.0f, rShift, uShift);
		Expect(Near(rShift, 11.95f, 0.2f) && Near(uShift, -0.96f, 0.2f),
			"NF Heavy flat-ground parallax shifts aim +12 HU right");
		MgParallaxAimShift(5, false, 800.0f, -1.0f, rShift, uShift);
		Expect(Near(rShift, 31.2f, 0.1f) && Near(uShift, -2.5f, 0.1f),
			"NF Heavy skyline (no hit behind) shifts by full -mgOffset");
		MgParallaxAimShift(5, false, 800.0f, 0.0f, rShift, uShift);
		Expect(Near(rShift, 0.0f) && Near(uShift, 0.0f),
			"wall directly behind target requires zero parallax shift");
		MgParallaxAimShift(11, false, 800.0f, 400.0f, rShift, uShift);
		Expect(rShift < -8.0f && rShift > -10.0f && Near(uShift, 0.0f),
			"BE Medium (+30 HU right MG) shifts aim left");
		MgParallaxAimShift(3, false, 800.0f, 400.0f, rShift, uShift);
		Expect(uShift < -5.0f && uShift > -8.0f,
			"NF Medium (+20 HU roof MG) shifts aim down");
		MgParallaxAimShift(5, true, 800.0f, 400.0f, rShift, uShift);
		Expect(Near(rShift, 0.0f), "NF Heavy dual-cannon centered MG has zero lateral parallax shift");
	}

	// 3. ChooseBallistics: MG hitscan, cannon 3300+drop, missile-only 2300 flat.
	{
		const BallisticsResult bInf = ChooseBallistics(900.0f, true, true, true);
		Expect(bInf.mode == BALLISTICS_HITSCAN && Near(bInf.flightTime, 0.0f) && Near(bInf.gravityDropZ, 0.0f),
			"infantry MG is hitscan with zero lead and zero drop");

		const BallisticsResult bCan = ChooseBallistics(3300.0f, false, true, true);
		Expect(bCan.mode == BALLISTICS_CANNON && Near(bCan.flightTime, 1.0f) && Near(bCan.gravityDropZ, 150.0f),
			"cannon at 3300 HU has t=1.0 s and drop=150 HU");

		const BallisticsResult bMis = ChooseBallistics(2300.0f, false, false, true);
		Expect(bMis.mode == BALLISTICS_MISSILE && Near(bMis.flightTime, 1.0f) && Near(bMis.gravityDropZ, 0.0f),
			"missile-only at 2300 HU has t=1.0 s and zero gravity drop");

		const float pos[3] = { 100.0f, 200.0f, 50.0f };
		const float vel[3] = { 150.0f, -50.0f, 0.0f };
		float aim[3] = {};
		ComputeLeadAimPoint(pos, vel, bInf, aim);
		Expect(Near(aim[0], 100.0f) && Near(aim[1], 200.0f) && Near(aim[2], 50.0f),
			"hitscan lead keeps exact target position");
		ComputeLeadAimPoint(pos, vel, bCan, aim);
		Expect(Near(aim[0], 250.0f) && Near(aim[1], 150.0f) && Near(aim[2], 200.0f),
			"cannon lead applies velocity + gravity compensation");
	}

	// 4. 1-tick fire-group switch guard.
	{
		// Already in desired group -> no switch, fire allowed.
		const FireGroupDecision same = DecideFireGroupSwitch(true, 1, 2, true, true, 10.0f, 0.0f);
		Expect(!same.sendSwitch && !same.suppressFireThisTick && same.wantGroup == 1,
			"infantry in group 2 fires immediately");

		// Group 1 -> Group 2 on infantry: sends switch AND suppresses fire this tick.
		const FireGroupDecision sw = DecideFireGroupSwitch(true, 0, 2, true, true, 10.0f, 0.0f);
		Expect(sw.sendSwitch && sw.suppressFireThisTick && sw.wantGroup == 1,
			"switch tick sends emp_change_vehicweap and suppresses stale-group fire");

		// Next tick (0.1 s later) while server has not yet updated m_iGroup: keep suppressing fire.
		const FireGroupDecision grace = DecideFireGroupSwitch(true, 0, 2, true, true, 10.1f, 10.0f);
		Expect(!grace.sendSwitch && grace.suppressFireThisTick,
			"grace window holds fire until group change is read back");

		// After grace window (0.5 s later) if group still did not change: do not deadlock.
		const FireGroupDecision timeout = DecideFireGroupSwitch(true, 0, 2, true, true, 10.5f, 10.0f);
		Expect(!timeout.sendSwitch && !timeout.suppressFireThisTick,
			"after grace window, fire is released instead of deadlocking");

		// Single-group or missiles off -> no switch, no suppression.
		const FireGroupDecision single = DecideFireGroupSwitch(true, 0, 1, true, true, 10.0f, 0.0f);
		Expect(!single.sendSwitch && !single.suppressFireThisTick, "single-group vehicle never switches");
	}

	// 5. Cross-group weapon & ammo summarization.
	{
		constexpr int kAttack = 1;
		constexpr int kAttack2 = 2048;
		std::int32_t weapons[8] = { 12, 0, 27, 255, 255, 255, 255, 255 }; // cannon, MG, missile
		std::int32_t ammo[8]    = {  0, 60,  4,   0,   0,   0,   0,   0 }; // cannon empty, missile loaded
		std::int32_t clips[8]   = {  0,  4,  3,   0,   0,   0,   0,   0 };
		std::uint8_t modes[8]   = {  0,  1,  0,   0,   0,   0,   0,   0 }; // MG=a1, cannon+missile=a2
		std::uint8_t groups[48] = {};
		// Group 0 (UI 1): cannon + MG + missile. Group 1 (UI 2): cannon + MG.
		groups[0 * 6 + 0] = 1; groups[0 * 6 + 1] = 1;
		groups[1 * 6 + 0] = 1; groups[1 * 6 + 1] = 1;
		groups[2 * 6 + 0] = 1;

		// Active group 1 (UI group 2, infantry): cannon is empty, missile is ONLY in group 0!
		const TurretWeaponSummary s2 = SummarizeTurretWeapons(weapons, ammo, clips, modes, groups, 1, kAttack, kAttack2);
		Expect(s2.valid && s2.weaponGroupCount == 2, "two groups detected");
		Expect(s2.mgPure == kAttack && s2.mgMixed == 0, "pure MG on IN_ATTACK in group 2");
		Expect(!s2.activeCannonLoaded && !s2.activeMissileLoaded, "neither cannon nor missile loaded in group 2");
		Expect(s2.cannonPresent && !s2.cannonLeft && s2.cannonTotal == 0, "cannon counted empty");
		Expect(s2.missilePresent && s2.missileLeft && s2.missileTotal == 7,
			"group-1 missiles still visible in cross-group totals while active group is 2");
		Expect(s2.heavyPresent && s2.heavyLeft && !ShouldRetreatForLoadoutOrAmmo(s2),
			"does NOT falsely trigger ammo retreat in group 2 while group 1 has missiles");

		// Active group 0 (UI group 1, armour): missile is loaded -> armour mask active, ballistics picks missile.
		const TurretWeaponSummary s1 = SummarizeTurretWeapons(weapons, ammo, clips, modes, groups, 0, kAttack, kAttack2);
		Expect(s1.armour == kAttack2 && !s1.activeCannonLoaded && s1.activeMissileLoaded,
			"group 1 sees loaded missile on IN_ATTACK2");
		const BallisticsResult bm = ChooseBallistics(1150.0f, false, s1.activeCannonLoaded, s1.activeMissileLoaded);
		Expect(bm.mode == BALLISTICS_MISSILE, "empty cannon + loaded missile selects flat missile ballistics");

		// Now empty missiles too -> ammo retreat triggers.
		ammo[2] = 0; clips[2] = 0;
		const TurretWeaponSummary sEmpty = SummarizeTurretWeapons(weapons, ammo, clips, modes, groups, 1, kAttack, kAttack2);
		Expect(sEmpty.heavyPresent && !sEmpty.heavyLeft && ShouldRetreatForLoadoutOrAmmo(sEmpty),
			"both cannon and missiles empty triggers ammo retreat");

		// Mixed button (MG + cannon both on mode 1) triggers loadout refit even with ammo left.
		ammo[0] = 40;
		modes[0] = 1;
		const TurretWeaponSummary sMixed = SummarizeTurretWeapons(weapons, ammo, clips, modes, groups, 0, kAttack, kAttack2);
		Expect(sMixed.mgMixed == kAttack && sMixed.mgPure == 0 && ShouldRetreatForLoadoutOrAmmo(sMixed),
			"shared MG+cannon button detected as mgMixed and triggers refit");
	}

	if (g_failures != 0)
	{
		std::fprintf(stderr, "%d failure(s)\n", g_failures);
		return 1;
	}
	std::printf("vehicle aim tests PASS\n");
	return 0;
}
