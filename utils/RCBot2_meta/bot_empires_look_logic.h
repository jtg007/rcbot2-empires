// Pure-C++ infantry realism core: decoupled spring-damper view turning,
// content-based look priorities, level navmesh lookahead, anti-wall idle
// direction selection, tactical reload, and combat fire discipline.
//
// SDK-free so it can be unit-tested offline (scripts/run_look_logic_tests.sh).
// Inspired by TF2 NextBot (NextBotPlayerBody), ReGameDLL_CS (cs_bot_vision),
// and YaPB (vision/combat/behavior).
#ifndef BOT_EMPIRES_LOOK_LOGIC_H
#define BOT_EMPIRES_LOOK_LOGIC_H

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace EmpiresLookLogic
{

constexpr float kPi = 3.14159265358979323846f;
constexpr float kDegToRad = kPi / 180.0f;
constexpr float kRadToDeg = 180.0f / kPi;

struct Vec3
{
	float x = 0.0f;
	float y = 0.0f;
	float z = 0.0f;
};

inline float NormalizeAngle(float deg)
{
	if (!std::isfinite(deg))
		return 0.0f;
	while (deg > 180.0f)
		deg -= 360.0f;
	while (deg <= -180.0f)
		deg += 360.0f;
	return deg;
}

inline float ClampFloat(float v, float lo, float hi)
{
	return (std::max)(lo, (std::min)(v, hi));
}

inline float AngleDiff(float targetDeg, float currentDeg)
{
	return NormalizeAngle(targetDeg - currentDeg);
}

inline float Length2D(const Vec3& a, const Vec3& b)
{
	const float dx = b.x - a.x;
	const float dy = b.y - a.y;
	return std::sqrt(dx * dx + dy * dy);
}

// -----------------------------------------------------------------------------
// Stage 1: Decoupled per-axis critically-damped spring-damper + look priorities
// -----------------------------------------------------------------------------

struct SpringAxis
{
	float velocity = 0.0f; // deg/s

	void Reset()
	{
		velocity = 0.0f;
	}
};

struct SpringConfig
{
	float stiffness = 200.0f;   // deg/s^2 per deg of error
	float damping = 28.28f;     // deg/s per deg/s (2*sqrt(k) = critically damped)
	float maxAccel = 3000.0f;   // deg/s^2
	float snapAngle = 1.0f;     // deg: snap and zero velocity inside this window
};

struct ViewSpringConfigs
{
	SpringConfig yaw;
	SpringConfig pitch;
};

// ReGameDLL/YaPB spring constants: critically damped yaw (no overshoot),
// 2x stiffer pitch (half angular range +/-90 vs +/-180), snappier in combat.
inline ViewSpringConfigs ComputeSpringConfigs(bool combat, bool urgent, float skill01)
{
	const float skill = ClampFloat(skill01, 0.0f, 1.0f);
	ViewSpringConfigs cfg;
	float yawStiff = 200.0f;
	float maxAccel = 3000.0f;
	if (combat || urgent)
	{
		yawStiff = 300.0f + 100.0f * skill;
		maxAccel = 3000.0f + 300.0f * skill;
	}
	cfg.yaw.stiffness = yawStiff;
	cfg.yaw.damping = 2.0f * std::sqrt(yawStiff); // zeta = 1.0 (critical damping)
	cfg.yaw.maxAccel = maxAccel;
	cfg.yaw.snapAngle = 1.0f;

	cfg.pitch.stiffness = 2.0f * yawStiff;
	cfg.pitch.damping = 0.85f * 2.0f * std::sqrt(cfg.pitch.stiffness);
	cfg.pitch.maxAccel = maxAccel;
	cfg.pitch.snapAngle = 1.0f;
	return cfg;
}

inline float StepSpringAxis(SpringAxis& axis, const SpringConfig& cfg, float currentDeg,
	float targetDeg, float dt)
{
	const float safeDt = ClampFloat(dt, 0.001f, 0.1f);
	const float err = AngleDiff(targetDeg, currentDeg);
	if (std::fabs(err) < cfg.snapAngle)
	{
		axis.velocity = 0.0f;
		return NormalizeAngle(targetDeg);
	}
	const float rawAccel = cfg.stiffness * err - cfg.damping * axis.velocity;
	const float accel = ClampFloat(rawAccel, -cfg.maxAccel, cfg.maxAccel);
	axis.velocity += safeDt * accel;
	if (!std::isfinite(axis.velocity))
		axis.velocity = 0.0f;
	return NormalizeAngle(currentDeg + safeDt * axis.velocity);
}

// Tracks whether the bot's head is steady (TF2 NextBot: < 100 deg/s for >= 0.3 s).
struct HeadSteadyTracker
{
	static constexpr float kSteadyMaxRateDegPerSec = 100.0f;
	static constexpr float kSettleDurationSec = 0.3f;

	bool initialized = false;
	float priorPitch = 0.0f;
	float priorYaw = 0.0f;
	float steadySince = -1.0f;

	void Reset()
	{
		initialized = false;
		priorPitch = 0.0f;
		priorYaw = 0.0f;
		steadySince = -1.0f;
	}

	void Update(float now, float dt, float pitchDeg, float yawDeg)
	{
		const float safeDt = ClampFloat(dt, 0.001f, 0.1f);
		if (!initialized)
		{
			initialized = true;
			priorPitch = pitchDeg;
			priorYaw = yawDeg;
			steadySince = now;
			return;
		}
		const float pitchRate = std::fabs(AngleDiff(pitchDeg, priorPitch)) / safeDt;
		const float yawRate = std::fabs(AngleDiff(yawDeg, priorYaw)) / safeDt;
		priorPitch = pitchDeg;
		priorYaw = yawDeg;
		if (pitchRate <= kSteadyMaxRateDegPerSec && yawRate <= kSteadyMaxRateDegPerSec)
		{
			if (steadySince < 0.0f)
				steadySince = now;
		}
		else
		{
			steadySince = -1.0f;
		}
	}

	bool IsSteady() const
	{
		return steadySince >= 0.0f;
	}

	float SteadyDuration(float now) const
	{
		return steadySince >= 0.0f ? (std::max)(0.0f, now - steadySince) : 0.0f;
	}
};

// Updates pitch and yaw independently with zero cross-axis momentum coupling.
inline void StepViewAngles(SpringAxis& pitchAxis, SpringAxis& yawAxis,
	float& pitchDeg, float& yawDeg, float targetPitchDeg, float targetYawDeg,
	float dt, bool combat, bool urgent, float skill01, bool freezePitch = false)
{
	const ViewSpringConfigs cfg = ComputeSpringConfigs(combat, urgent, skill01);
	yawDeg = StepSpringAxis(yawAxis, cfg.yaw, yawDeg, targetYawDeg, dt);
	if (freezePitch)
	{
		pitchAxis.velocity = 0.0f;
	}
	else
	{
		const float clampedTargetPitch = ClampFloat(targetPitchDeg, -89.0f, 89.0f);
		pitchDeg = StepSpringAxis(pitchAxis, cfg.pitch, pitchDeg, clampedTargetPitch, dt);
		pitchDeg = ClampFloat(pitchDeg, -89.0f, 89.0f);
	}
}

// Content-based look priorities (TF2 NextBot IBody::LookAtPriorityType).
enum class LookPriority : int
{
	BORING = 0,       // idle, walking path
	INTERESTING = 1,  // last seen enemy, sound/noise
	IMPORTANT = 2,    // explicit action target (kit, button, grenade, ladder, build)
	URGENT = 3,       // hurt origin (unseen shooter)
	CRITICAL = 4      // active visible enemy
};

// Arbitrates look-task changes by semantic priority and head steadiness rather
// than execution-phase order alone.
inline bool ShouldAcceptLookRequest(LookPriority currentPri, float currentExpireAt,
	LookPriority newPri, bool sameTask, float now, bool headSteady, float headSteadyDur)
{
	if (newPri > currentPri)
		return true;
	if (newPri < currentPri)
		return now >= currentExpireAt;
	// Same priority: updating/extending the same task always succeeds.
	if (sameTask)
		return true;
	// Switching between different tasks of equal priority: wait for expiry and
	// (outside active combat) a brief head settle so the view does not spaz.
	if (now < currentExpireAt)
		return false;
	if (newPri < LookPriority::URGENT && (!headSteady || headSteadyDur < HeadSteadyTracker::kSettleDurationSec))
		return false;
	return true;
}

// Subtle per-bot phase-shifted idle breathing/glance drift (YaPB cv_aim_drift).
inline void ComputeIdleDrift(float now, int botSeed, bool sniper, float& outYawDeg, float& outPitchDeg)
{
	const float scale = sniper ? 0.25f : 1.0f;
	const float phase = static_cast<float>(botSeed) * 37.0f * kDegToRad;
	outYawDeg = scale * 2.2f * std::cos(0.45f * now + phase);
	outPitchDeg = scale * 0.7f * std::sin(0.31f * now + phase * 0.5f);
}

// -----------------------------------------------------------------------------
// Stage 2: Level Navmesh Lookahead + Corner/LOS Hold
// -----------------------------------------------------------------------------

constexpr float kDefaultNavLookaheadDist = 220.0f;
constexpr float kMaxLevelSlopePitchDeg = 12.0f;
constexpr float kLastEnemyHoldSec = 2.5f;
constexpr float kLastEnemyChestLift = 48.0f;

// Computes a smooth lookahead target along a navmesh polyline, keeping gaze
// level on flat ground and gentle slopes (YaPB d64e41c) while allowing mild
// pitch (+/-12 deg) on steep ramps and full vertical look on ladders.
inline bool PickNavLookaheadPoint(const Vec3& botOrigin, const Vec3& eyePos,
	const Vec3* points, std::size_t count, std::size_t currentIndex,
	float lookaheadDist, bool onLadderOrVertical, Vec3& outLook)
{
	if (points == nullptr || count == 0 || currentIndex >= count)
		return false;

	const float wantDist = (std::max)(48.0f, lookaheadDist);
	Vec3 prev = botOrigin;
	Vec3 chosen = points[currentIndex];
	float accum = 0.0f;
	bool reached = false;

	for (std::size_t i = currentIndex; i < count; ++i)
	{
		const Vec3& pt = points[i];
		const float seg = Length2D(prev, pt);
		if (accum + seg >= wantDist && seg > 1.0f)
		{
			const float frac = ClampFloat((wantDist - accum) / seg, 0.0f, 1.0f);
			chosen.x = prev.x + (pt.x - prev.x) * frac;
			chosen.y = prev.y + (pt.y - prev.y) * frac;
			chosen.z = prev.z + (pt.z - prev.z) * frac;
			reached = true;
			break;
		}
		accum += seg;
		prev = pt;
		chosen = pt;
	}

	const float horiz = Length2D(eyePos, chosen);
	if (!reached && horiz < 32.0f)
		return false;

	if (onLadderOrVertical)
	{
		outLook = Vec3{ chosen.x, chosen.y, chosen.z + kLastEnemyChestLift };
		return true;
	}

	// Keep gaze level with our eyes on flat ground and gentle slopes; on steep
	// ramps, follow the slope up to +/-12 deg so the bot never stares at the floor.
	const float eyeHeightOffset = eyePos.z - botOrigin.z;
	const float rawTargetZ = chosen.z + eyeHeightOffset;
	const float dz = rawTargetZ - eyePos.z;
	float lookZ = eyePos.z;
	if (horiz > 48.0f && std::fabs(dz) > 24.0f)
	{
		const float maxDz = horiz * std::tan(kMaxLevelSlopePitchDeg * kDegToRad);
		lookZ = eyePos.z + ClampFloat(dz, -maxDz, maxDz);
	}
	outLook = Vec3{ chosen.x, chosen.y, lookZ };
	return true;
}

inline bool ShouldHoldLastEnemyLook(bool selfSighted, float now, float lastSeenTime,
	float holdDuration, float dist2D)
{
	if (!selfSighted || lastSeenTime <= 0.0f || dist2D < 48.0f)
		return false;
	const float age = now - lastSeenTime;
	return age >= 0.0f && age <= holdDuration;
}

// Bounds CFindLastEnemy velocity extrapolation (legacy used vLast + vVel * 10 s).
inline Vec3 ClampLastEnemyExtrapolation(const Vec3& lastPos, const Vec3& vel,
	float maxSeconds = 1.2f, float maxDist2D = 240.0f)
{
	float dx = vel.x * maxSeconds;
	float dy = vel.y * maxSeconds;
	const float len = std::sqrt(dx * dx + dy * dy);
	if (len > maxDist2D && len > 1.0f)
	{
		const float scale = maxDist2D / len;
		dx *= scale;
		dy *= scale;
	}
	return Vec3{ lastPos.x + dx, lastPos.y + dy, lastPos.z };
}

// -----------------------------------------------------------------------------
// Stage 3: Anti-Wall Smart Idle Look (YaPB PR #800 + ReGameDLL open corridors)
// -----------------------------------------------------------------------------

constexpr float kIdleMinWallClearance = 140.0f;
constexpr float kIdleMaxRayDist = 600.0f;

struct IdleRaySample
{
	float yawDeg = 0.0f;
	float clearDist = 0.0f;
	float threatBias = 0.0f; // 0..1 bonus when aligned with front/last threat
};

// Selects an open horizontal direction when standing/holding, rejecting walls
// closer than minWallClearance and gently preferring open sightlines + threat
// direction without whipping 180 deg unless currently facing a wall.
inline bool PickSmartIdleYaw(const IdleRaySample* samples, std::size_t count,
	float currentYawDeg, float currentClearDist, float minWallClearance,
	float jitterSeed01, float& outYawDeg)
{
	if (samples == nullptr || count == 0)
		return false;

	const bool facingWall = currentClearDist < minWallClearance;
	bool found = false;
	float bestScore = -1.0e9f;
	float bestYaw = currentYawDeg;

	// Fallback: farthest ray even if every direction is tight (narrow alcove).
	float widestDist = -1.0f;
	float widestYaw = currentYawDeg;

	for (std::size_t i = 0; i < count; ++i)
	{
		const IdleRaySample& s = samples[i];
		if (s.clearDist > widestDist)
		{
			widestDist = s.clearDist;
			widestYaw = s.yawDeg;
		}
		if (s.clearDist < minWallClearance)
			continue;

		const float openness = ClampFloat(s.clearDist / kIdleMaxRayDist, 0.0f, 1.0f);
		const float turnDeg = std::fabs(AngleDiff(s.yawDeg, currentYawDeg));
		// When not facing a wall, favour moderate scans (15..75 deg) over staying
		// frozen at 0 deg or snapping 180 deg behind.
		float turnScore = 0.0f;
		if (!facingWall)
		{
			if (turnDeg < 10.0f)
				turnScore = -0.15f;
			else if (turnDeg <= 80.0f)
				turnScore = 0.20f;
			else
				turnScore = -0.25f * ((turnDeg - 80.0f) / 100.0f);
		}
		// Deterministic small tie-breaker so bots cycle across open corridors.
		const float phase = std::sin(static_cast<float>(i) * 2.17f + jitterSeed01 * 6.28318f) * 0.12f;
		const float score = openness * 0.65f + ClampFloat(s.threatBias, 0.0f, 1.0f) * 0.45f + turnScore + phase;
		if (!found || score > bestScore)
		{
			found = true;
			bestScore = score;
			bestYaw = s.yawDeg;
		}
	}

	outYawDeg = NormalizeAngle(found ? bestYaw : widestYaw);
	return found || widestDist > 0.0f;
}

// -----------------------------------------------------------------------------
// Stage 4: Tactical Reload + Asymmetric Combat Fire Discipline
// -----------------------------------------------------------------------------

constexpr float kTacticalReloadQuietSec = 2.5f;
constexpr float kTacticalReloadThreshold = 0.60f; // user 27.11.: 40 % fired almost never
constexpr float kCombatFireFallbackSec = 0.35f;
constexpr float kLongRangeBurstDist = 750.0f;
constexpr float kBurstFireSec = 0.55f;
constexpr float kBurstPauseSec = 0.18f;

inline bool ShouldTacticalReload(bool hasVisibleEnemy, float timeSinceEnemySeen,
	int clip1, int maxClip1, int reserveAmmo, bool busyAction, bool meleeOrExplosive)
{
	if (hasVisibleEnemy || busyAction || meleeOrExplosive)
		return false;
	if (timeSinceEnemySeen < kTacticalReloadQuietSec)
		return false;
	if (clip1 <= 0 || maxClip1 <= 1 || reserveAmmo <= 0)
		return false;
	return static_cast<float>(clip1) < static_cast<float>(maxClip1) * kTacticalReloadThreshold;
}

// Asymmetric fire cone (YaPB FocusEnemy + safety valve):
// - Point blank (< 110 HU) or melee: always fire.
// - Close (< 180 HU): fire from dot >= 0.80 (~37 deg).
// - Mid/long: if enemy faces us (enemyDot >= 0.90), fire from dot >= 0.92 (~23 deg)
//   so duel reaction never degrades; if enemy faces away, wait until dot >= 0.985 (~10 deg)
//   for a clean opening shot. Safety valve releases after 0.35 s if roughly aimed (>= 0.85).
inline bool ShouldAllowCombatFire(float dist2D, float viewDotToEnemy, float enemyDotToMe,
	float offTargetElapsed, bool melee, bool scoped)
{
	if (melee || dist2D < 110.0f)
		return true;
	if (dist2D < 180.0f && !scoped)
		return viewDotToEnemy >= 0.80f;

	const bool enemyFacingMe = enemyDotToMe >= 0.90f;
	const float requiredDot = scoped ? 0.99f : (enemyFacingMe ? 0.92f : 0.985f);
	if (viewDotToEnemy >= requiredDot)
		return true;
	if (!scoped && offTargetElapsed >= kCombatFireFallbackSec && viewDotToEnemy >= 0.85f)
		return true;
	return false;
}

// Returns true when automatic fire at long range should briefly pause to reset spread.
inline bool ShouldPauseForLongRangeBurst(float dist2D, bool automaticWeapon, bool scoped,
	float cycleElapsedSec)
{
	if (!automaticWeapon || scoped || dist2D <= kLongRangeBurstDist)
		return false;
	const float period = kBurstFireSec + kBurstPauseSec;
	const float pos = std::fmod((std::max)(0.0f, cycleElapsedSec), period);
	return pos >= kBurstFireSec;
}

} // namespace EmpiresLookLogic

#endif // BOT_EMPIRES_LOOK_LOGIC_H
