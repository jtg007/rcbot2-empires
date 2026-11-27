#include "bot_empires_look_logic.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

using namespace EmpiresLookLogic;

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

void TestSpringDecouplingAndConvergence()
{
	SpringAxis pitchAxis, yawAxis;
	float pitch = 0.0f;
	float yaw = 0.0f;
	float maxPitchDrift = 0.0f;
	float maxYawOvershoot = 0.0f;
	const float dt = 1.0f / 66.0f;

	// 90-degree combat yaw turn with 0 pitch target must NEVER disturb pitch
	// (regression test for the legacy shared m_fAimMoment bug) and must settle
	// quickly without overshoot.
	for (int tick = 0; tick < 66; ++tick)
	{
		StepViewAngles(pitchAxis, yawAxis, pitch, yaw, 0.0f, 90.0f, dt, true, false, 0.8f);
		maxPitchDrift = (std::max)(maxPitchDrift, std::fabs(pitch));
		if (yaw > 90.0f)
			maxYawOvershoot = (std::max)(maxYawOvershoot, yaw - 90.0f);
	}
	Expect(maxPitchDrift < 0.001f, "90-deg yaw turn must produce 0 pitch drift");
	Expect(std::fabs(AngleDiff(90.0f, yaw)) < 1.5f, "combat yaw turn must converge within 1 s");
	Expect(maxYawOvershoot < 2.0f, "critically damped yaw must not overshoot > 2 deg");

	// Fast combat convergence check: within 0.30 s of a 60-deg target jump, error < 10 deg.
	pitchAxis.Reset();
	yawAxis.Reset();
	pitch = 0.0f;
	yaw = 0.0f;
	for (int tick = 0; tick < 20; ++tick) // 20/66 ~= 0.30 s
		StepViewAngles(pitchAxis, yawAxis, pitch, yaw, 0.0f, 60.0f, dt, true, false, 0.8f);
	Expect(std::fabs(AngleDiff(60.0f, yaw)) < 10.0f, "combat 60-deg turn should get within 10 deg in 0.30 s");

	// Wrap-around across +/-180 boundary takes the short 20-deg path.
	pitchAxis.Reset();
	yawAxis.Reset();
	yaw = 170.0f;
	for (int tick = 0; tick < 15; ++tick)
		StepViewAngles(pitchAxis, yawAxis, pitch, yaw, 0.0f, -170.0f, dt, true, false, 0.8f);
	Expect(std::fabs(AngleDiff(-170.0f, yaw)) < 5.0f, "yaw wrap across +/-180 must take shortest path");
	Expect(yaw > 165.0f || yaw < -165.0f, "yaw wrap must stay near 180 boundary");
}

void TestLookPrioritiesAndHeadSteady()
{
	// CRITICAL (enemy) always overrides URGENT (hurt) even before expiry.
	Expect(ShouldAcceptLookRequest(LookPriority::URGENT, 15.0f, LookPriority::CRITICAL, false, 10.0f, false, 0.0f),
		"CRITICAL enemy must immediately override unexpired URGENT hurt look");
	// BORING (idle/path) cannot override INTERESTING (last enemy / noise) before expiry.
	Expect(!ShouldAcceptLookRequest(LookPriority::INTERESTING, 12.0f, LookPriority::BORING, false, 10.0f, true, 1.0f),
		"BORING must not override unexpired INTERESTING");
	// BORING succeeds once INTERESTING expired.
	Expect(ShouldAcceptLookRequest(LookPriority::INTERESTING, 9.5f, LookPriority::BORING, false, 10.0f, true, 1.0f),
		"BORING allowed once INTERESTING expired");
	// Equal priority different task blocked while head unsteady or settled < 0.3 s.
	Expect(!ShouldAcceptLookRequest(LookPriority::BORING, 9.0f, LookPriority::BORING, false, 10.0f, false, 0.0f),
		"equal priority switch rejected while head unsteady");
	Expect(!ShouldAcceptLookRequest(LookPriority::BORING, 9.0f, LookPriority::BORING, false, 10.0f, true, 0.1f),
		"equal priority switch rejected before settle duration");
	Expect(ShouldAcceptLookRequest(LookPriority::BORING, 9.0f, LookPriority::BORING, false, 10.0f, true, 0.4f),
		"equal priority switch allowed after head settles");
	// Same task at same priority always allowed to refresh.
	Expect(ShouldAcceptLookRequest(LookPriority::CRITICAL, 12.0f, LookPriority::CRITICAL, true, 10.0f, false, 0.0f),
		"same task always allowed to refresh");

	HeadSteadyTracker tracker;
	tracker.Update(1.0f, 0.05f, 0.0f, 0.0f);
	tracker.Update(1.05f, 0.05f, 0.0f, 20.0f); // 400 deg/s -> unsteady
	Expect(!tracker.IsSteady(), "fast 400 deg/s turn must mark head unsteady");
	tracker.Update(1.10f, 0.05f, 0.0f, 20.5f); // 10 deg/s -> steady starts
	Expect(tracker.IsSteady(), "slow 10 deg/s motion must mark head steady");
	tracker.Update(1.45f, 0.05f, 0.0f, 20.5f);
	Expect(tracker.SteadyDuration(1.45f) >= 0.3f, "steady duration must accumulate to >= 0.3 s");
}

void TestNavLookaheadAndSlopes()
{
	// Flat path with 90-deg corner: lookahead interpolates past the first point.
	const Vec3 bot{ 0.0f, 0.0f, 0.0f };
	const Vec3 eye{ 0.0f, 0.0f, 64.0f };
	const Vec3 flatPath[] = { { 100.0f, 0.0f, 0.0f }, { 100.0f, 250.0f, 0.0f } };
	Vec3 look;
	Expect(PickNavLookaheadPoint(bot, eye, flatPath, 2, 0, 220.0f, false, look),
		"flat path lookahead must succeed");
	Expect(std::fabs(look.x - 100.0f) < 1.0f && std::fabs(look.y - 120.0f) < 1.0f,
		"lookahead should interpolate 120 HU around the corner");
	Expect(std::fabs(look.z - eye.z) < 0.01f, "flat path lookahead z must equal eye z");

	// Gentle downhill slope (drop 20 HU over 220 HU): gaze stays level at eye.z.
	const Vec3 gentleSlope[] = { { 220.0f, 0.0f, -20.0f } };
	Expect(PickNavLookaheadPoint(bot, eye, gentleSlope, 1, 0, 220.0f, false, look),
		"gentle slope lookahead must succeed");
	Expect(std::fabs(look.z - eye.z) < 0.01f, "gentle slope must keep lookahead level at eye height");

	// Steep ramp (rise 140 HU over 220 HU ~= 32 deg): pitch clamped to <= 12 deg.
	const Vec3 steepRamp[] = { { 220.0f, 0.0f, 140.0f } };
	Expect(PickNavLookaheadPoint(bot, eye, steepRamp, 1, 0, 220.0f, false, look),
		"steep ramp lookahead must succeed");
	const float pitchDeg = std::atan2(look.z - eye.z, Length2D(eye, look)) * kRadToDeg;
	Expect(pitchDeg > 11.0f && pitchDeg <= 12.1f, "steep ramp pitch must be clamped to +12 deg");

	// Ladder mode: looks vertically up at the ladder top.
	Expect(PickNavLookaheadPoint(bot, eye, steepRamp, 1, 0, 220.0f, true, look),
		"ladder lookahead must succeed");
	Expect(look.z > 180.0f, "ladder lookahead must preserve vertical target");

	// Extrapolation clamp: 300 u/s enemy should not extrapolate 3000 HU (legacy *10s).
	const Vec3 extrap = ClampLastEnemyExtrapolation(Vec3{ 0, 0, 0 }, Vec3{ 300.0f, 0.0f, 0.0f });
	Expect(extrap.x > 200.0f && extrap.x <= 240.1f, "extrapolation must clamp to <= 240 HU");
}

void TestSmartIdleLook()
{
	// Bot facing north (90 deg) directly into a wall (40 HU); south (-90) and east (0) are open.
	const IdleRaySample samples[] = {
		{ 90.0f, 40.0f, 0.0f },
		{ 45.0f, 60.0f, 0.0f },
		{ 0.0f, 550.0f, 0.8f },
		{ -45.0f, 400.0f, 0.2f },
		{ -90.0f, 500.0f, 0.0f },
		{ -135.0f, 90.0f, 0.0f },
		{ 180.0f, 80.0f, 0.0f },
		{ 135.0f, 50.0f, 0.0f },
	};
	float outYaw = 90.0f;
	Expect(PickSmartIdleYaw(samples, 8, 90.0f, 40.0f, kIdleMinWallClearance, 0.2f, outYaw),
		"smart idle look must find an open direction");
	Expect(std::fabs(AngleDiff(0.0f, outYaw)) < 1.0f,
		"smart idle look must pick open threat-aligned corridor instead of wall");

	// All directions tight (< 140 HU): falls back to widest ray rather than failing.
	const IdleRaySample alcove[] = {
		{ 0.0f, 50.0f, 0.0f },
		{ 90.0f, 110.0f, 0.0f },
		{ 180.0f, 40.0f, 0.0f },
	};
	Expect(PickSmartIdleYaw(alcove, 3, 0.0f, 50.0f, kIdleMinWallClearance, 0.0f, outYaw),
		"alcove fallback must succeed");
	Expect(std::fabs(AngleDiff(90.0f, outYaw)) < 1.0f, "alcove fallback must choose widest ray");
}

void TestCombatPolish()
{
	// Tactical reload: quiet 3 s, 8/30 rounds, reserve 90 -> reload.
	Expect(ShouldTacticalReload(false, 3.0f, 8, 30, 90, false, false),
		"quiet bot with 8/30 rounds should reload tactically");
	// Active combat or >= 60% clip or no reserve -> no reload; 50% reloads.
	Expect(!ShouldTacticalReload(true, 3.0f, 8, 30, 90, false, false),
		"must not tactical-reload while enemy is visible");
	Expect(!ShouldTacticalReload(false, 1.0f, 8, 30, 90, false, false),
		"must wait 2.5 s quiet before tactical reload");
	Expect(ShouldTacticalReload(false, 3.0f, 15, 30, 90, false, false),
		"tactical-reload at 50% clip");
	Expect(!ShouldTacticalReload(false, 3.0f, 18, 30, 90, false, false),
		"must not tactical-reload at 60% clip");
	Expect(!ShouldTacticalReload(false, 3.0f, 8, 30, 0, false, false),
		"must not tactical-reload with 0 reserve");

	// Fire discipline:
	// 1. Point blank -> always allowed.
	Expect(ShouldAllowCombatFire(80.0f, 0.2f, 0.0f, 0.0f, false, false),
		"point blank fire always allowed");
	// 2. Mid-range while still turning 60 deg away (dot=0.5) -> hold fire.
	Expect(!ShouldAllowCombatFire(500.0f, 0.5f, 0.95f, 0.1f, false, false),
		"must hold fire when 60 deg off target");
	// 3. Duel (enemy facing us, dot=0.94 ~= 20 deg) -> fire promptly!
	Expect(ShouldAllowCombatFire(500.0f, 0.94f, 0.95f, 0.0f, false, false),
		"must fire promptly at 20 deg when enemy faces us");
	// 4. Unaware enemy (enemyDot=0.0) at 20 deg (dot=0.94) -> wait for cleaner shot,
	//    then fire at 7 deg (dot=0.992).
	Expect(!ShouldAllowCombatFire(500.0f, 0.94f, 0.0f, 0.1f, false, false),
		"against unaware enemy, wait for tighter alignment than 20 deg");
	Expect(ShouldAllowCombatFire(500.0f, 0.992f, 0.0f, 0.1f, false, false),
		"against unaware enemy, fire once within ~7 deg");
	// 5. Safety valve after 0.35 s at dot=0.88 -> fire so bot never stalls.
	Expect(ShouldAllowCombatFire(500.0f, 0.88f, 0.0f, 0.40f, false, false),
		"safety valve must release fire after 0.35 s when roughly on target");

	// Long-range burst pause only > 750 HU for automatic weapons.
	Expect(!ShouldPauseForLongRangeBurst(500.0f, true, false, 0.60f),
		"no burst pause at mid-range 500 HU");
	Expect(!ShouldPauseForLongRangeBurst(900.0f, true, false, 0.30f),
		"actively firing during 0..0.55 s burst window");
	Expect(ShouldPauseForLongRangeBurst(900.0f, true, false, 0.62f),
		"brief recoil pause during 0.55..0.73 s window at 900 HU");
	Expect(!ShouldPauseForLongRangeBurst(900.0f, true, false, 0.80f),
		"next burst resumes after 0.73 s");
}

} // namespace

int main()
{
	TestSpringDecouplingAndConvergence();
	TestLookPrioritiesAndHeadSteady();
	TestNavLookaheadAndSlopes();
	TestSmartIdleLook();
	TestCombatPolish();
	if (g_failures != 0)
	{
		std::fprintf(stderr, "look_logic_tests: %d FAILED\n", g_failures);
		return 1;
	}
	std::printf("look_logic_tests: PASS\n");
	return 0;
}
