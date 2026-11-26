// Pure-C++ turret alignment gating, cannon/missile/MG ballistics, 1-tick fire-group switch guard,
// and cross-group turret weapon/ammo summarization (TODO 3.21 / 3.21a / 3.10f).
// SDK-free so it can be unit-tested offline (scripts/run_vehicle_aim_tests.sh).
#ifndef BOT_EMPIRES_VEHICLE_AIM_H
#define BOT_EMPIRES_VEHICLE_AIM_H

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace EmpiresVehicleAim
{
// Turret alignment thresholds (3.21a).
constexpr float kMinAimSettle = 0.4f;           // allow faster fire once turret is aligned
constexpr float kLegacyAimDelay = 1.2f;         // fallback delay when no rotatable turret exists
constexpr float kFallbackFireTimeout = 2.5f;    // escalate & release fire if turret stalls
constexpr float kMgAlignToleranceDeg = 8.0f;    // tighter yaw window for hitscan MG on infantry
constexpr float kCannonAlignToleranceDeg = 12.0f;

// Projectile ballistics from scripts/vehicle_weapons.txt (RE vehicle-driving.md).
constexpr float kCannonSpeed = 3300.0f;
constexpr float kCannonGravityScale = 0.5f;
constexpr float kWorldGravity = 600.0f;
constexpr float kMissileSpeed = 2300.0f;        // Standard Missile Launcher (ID 27), Dumb Missile 1 (no drop)

// Fire-group switching (3.10f).
constexpr float kFireGroupCooldown = 1.0f;
constexpr float kFireGroupSwitchGrace = 0.35f;  // hold fire while emp_change_vehicweap takes effect

inline float NormalizeAngleDeg(float deg)
{
	while (deg > 180.0f)
		deg -= 360.0f;
	while (deg < -180.0f)
		deg += 360.0f;
	return deg;
}

inline float YawErrorDeg(float currentYawDeg, float targetYawDeg)
{
	return std::fabs(NormalizeAngleDeg(currentYawDeg - targetYawDeg));
}

inline bool HasRotatableTurret(bool turretValid, float minYaw, float maxYaw)
{
	return turretValid && std::fabs(maxYaw - minYaw) > 1.0f;
}

struct TurretAlignInput
{
	bool alignCheckEnabled = true;
	bool hasRotatableTurret = true;
	float turretYawDeg = 0.0f;   // turret models face local +X (runs 121856/121916)
	float wantYawDeg = 0.0f;
	float aimedForSeconds = 0.0f;
	bool infantryTarget = false;
};

struct TurretAlignResult
{
	bool canFire = false;
	bool fallbackEscalated = false;
	float yawErrorDeg = 0.0f;
};

// Gates vehicle firing on actual turret yaw alignment instead of a blind 1.2 s timer.
// Falls back cleanly when the chassis has no rotatable turret or after kFallbackFireTimeout.
inline TurretAlignResult TurretAlignedToFire(const TurretAlignInput& in)
{
	TurretAlignResult out;
	out.yawErrorDeg = YawErrorDeg(in.turretYawDeg, in.wantYawDeg);
	if (!in.alignCheckEnabled || !in.hasRotatableTurret)
	{
		out.canFire = in.aimedForSeconds > kLegacyAimDelay;
		return out;
	}
	if (in.aimedForSeconds <= kMinAimSettle)
		return out;
	const float tolerance = in.infantryTarget ? kMgAlignToleranceDeg : kCannonAlignToleranceDeg;
	if (out.yawErrorDeg <= tolerance)
	{
		out.canFire = true;
		return out;
	}
	if (in.aimedForSeconds >= kFallbackFireTimeout)
	{
		out.canFire = true;
		out.fallbackEscalated = true;
		return out;
	}
	return out;
}

enum BallisticsMode
{
	BALLISTICS_HITSCAN = 0,
	BALLISTICS_CANNON,
	BALLISTICS_MISSILE
};

struct BallisticsResult
{
	BallisticsMode mode = BALLISTICS_HITSCAN;
	float flightTime = 0.0f;
	float gravityDropZ = 0.0f;
};

// Selects hitscan (no lead) for MG vs infantry, straight 2300 u/s when only missiles are loaded,
// and 3300 u/s with 0.5x gravity drop for tank cannons.
inline BallisticsResult ChooseBallistics(float range, bool infantryTarget,
	bool cannonLoaded, bool missileLoaded)
{
	BallisticsResult out;
	if (infantryTarget || !(range > 0.0f))
	{
		out.mode = BALLISTICS_HITSCAN;
		return out;
	}
	if (!cannonLoaded && missileLoaded)
	{
		out.mode = BALLISTICS_MISSILE;
		out.flightTime = range / kMissileSpeed;
		out.gravityDropZ = 0.0f;
		return out;
	}
	out.mode = BALLISTICS_CANNON;
	out.flightTime = range / kCannonSpeed;
	out.gravityDropZ = 0.5f * kCannonGravityScale * kWorldGravity * out.flightTime * out.flightTime;
	return out;
}

inline void ComputeLeadAimPoint(const float targetPos[3], const float targetVel[3],
	const BallisticsResult& b, float outAim[3])
{
	outAim[0] = targetPos[0] + targetVel[0] * b.flightTime;
	outAim[1] = targetPos[1] + targetVel[1] * b.flightTime;
	outAim[2] = targetPos[2] + targetVel[2] * b.flightTime + b.gravityDropZ;
}

struct FireGroupDecision
{
	bool sendSwitch = false;
	int wantGroup = 0;               // 0-based group index (emp_change_vehicweap uses wantGroup + 1)
	bool suppressFireThisTick = false;
};

// 3.10f 1-tick fire-group switch guard:
// Group 0 (UI group 1) = cannon + missile + MG (against vehicles/buildings).
// Group 1 (UI group 2) = cannon + MG (against infantry, preserving missiles).
// Suppresses firing during the switch tick and the short grace window until ReadVehicleWeapons
// observes the new group; releases after kFireGroupSwitchGrace so a stuck/empty group cannot deadlock.
inline FireGroupDecision DecideFireGroupSwitch(bool weaponsKnown, int activeGroup, int groupCount,
	bool missilesEnabled, bool infantryTarget, float now, float lastSwitchSentAt)
{
	FireGroupDecision out;
	out.wantGroup = activeGroup;
	if (!weaponsKnown || groupCount < 2 || !missilesEnabled)
		return out;

	const int want = infantryTarget ? 1 : 0;
	out.wantGroup = want;
	if (activeGroup == want)
		return out;

	const float sinceSent = now - lastSwitchSentAt;
	if (sinceSent >= kFireGroupCooldown)
	{
		out.sendSwitch = true;
		out.suppressFireThisTick = true;
		return out;
	}
	if (sinceSent >= 0.0f && sinceSent < kFireGroupSwitchGrace)
		out.suppressFireThisTick = true;
	return out;
}

struct TurretWeaponSummary
{
	bool valid = false;
	int activeGroup = -1;
	int weaponGroupCount = 0;
	bool groupHasWeapon[6] = {};

	// Active-group button masks (caller passes IN_ATTACK / IN_ATTACK2 bit values).
	int mgPure = 0;
	int mgMixed = 0;
	int heavyInf = 0;
	int armour = 0;
	bool activeMgLeft = false;
	bool activeCannonLoaded = false;
	bool activeMissileLoaded = false;

	// Cross-group inventory & ammo totals (across all 8 slots / 6 groups so standing in group 2
	// never hides group-1 missiles or causes spurious ammo retreats).
	bool cannonPresent = false;
	bool cannonLeft = false;
	int cannonTotal = 0;
	bool missilePresent = false;
	bool missileLeft = false;
	int missileTotal = 0;
	bool heavyPresent = false;
	bool heavyLeft = false;
	int heavyTotal = 0;
	bool anyMgLeft = false;
};

inline bool IsMgWeaponId(int id)
{
	return id >= 0 && id <= 11;
}

inline bool IsArmourPiercingMgId(int id)
{
	return id == 1 || id == 4 || id == 5 || (id >= 8 && id <= 11);
}

inline bool IsCannonWeaponId(int id)
{
	return id >= 12 && id <= 22;
}

inline bool IsMissileWeaponId(int id)
{
	return id >= 27 && id <= 39;
}

inline bool IsGrenadeLauncherWeaponId(int id)
{
	return id >= 40 && id <= 42;
}

// Summarizes DT_EmpTankTurret weapon arrays into active-group fire masks and cross-group ammo totals.
inline TurretWeaponSummary SummarizeTurretWeapons(const std::int32_t weapons[8],
	const std::int32_t ammo[8], const std::int32_t clips[8], const std::uint8_t modes[8],
	const std::uint8_t groups[48], int activeGroup, int attackMask = 1, int attack2Mask = 2)
{
	TurretWeaponSummary out;
	if (activeGroup < 0 || activeGroup > 5)
		return out;

	out.activeGroup = activeGroup;
	int otherMask = 0;
	int mgLoadedMask = 0;

	for (int i = 0; i < 8; ++i)
	{
		const int id = weapons[i];
		if (id < 0 || id > 42)
			continue;

		bool assignedAnyGroup = false;
		for (int g = 0; g < 6; ++g)
		{
			if (groups[i * 6 + g] != 0)
			{
				assignedAnyGroup = true;
				out.groupHasWeapon[g] = true;
				if (g + 1 > out.weaponGroupCount)
					out.weaponGroupCount = g + 1;
			}
		}
		if (!assignedAnyGroup)
			continue;

		const int a = (std::max)(0, static_cast<int>(ammo[i]));
		const int c = (std::max)(0, static_cast<int>(clips[i]));
		const bool loaded = a > 0;
		const bool hasLeft = a > 0 || c > 0;

		// Cross-group totals (independent of activeGroup).
		if (IsMgWeaponId(id))
		{
			if (hasLeft)
				out.anyMgLeft = true;
		}
		else if (IsCannonWeaponId(id))
		{
			out.cannonPresent = true;
			out.cannonTotal += a + c;
			if (hasLeft)
				out.cannonLeft = true;
		}
		else if (IsMissileWeaponId(id))
		{
			out.missilePresent = true;
			out.missileTotal += a + c;
			if (hasLeft)
				out.missileLeft = true;
		}

		// Active-group fire masks.
		if (groups[i * 6 + activeGroup] == 0)
			continue;

		const int button = modes[i] != 0 ? attackMask : attack2Mask;
		if (IsMgWeaponId(id))
		{
			if (loaded)
				mgLoadedMask |= button;
			if (hasLeft)
				out.activeMgLeft = true;
		}
		else if (hasLeft)
		{
			otherMask |= button; // an empty missile pod must not block the MG on its button
		}

		if (loaded && IsCannonWeaponId(id))
			out.activeCannonLoaded = true;
		if (loaded && IsMissileWeaponId(id))
			out.activeMissileLoaded = true;
		if (loaded && (IsCannonWeaponId(id) || IsGrenadeLauncherWeaponId(id)))
			out.heavyInf |= button;
		if (loaded && (IsCannonWeaponId(id) || IsMissileWeaponId(id) || IsArmourPiercingMgId(id)))
			out.armour |= button;
	}

	out.mgPure = mgLoadedMask & ~otherMask;
	out.mgMixed = mgLoadedMask & otherMask;
	out.heavyPresent = out.cannonPresent || out.missilePresent;
	out.heavyLeft = out.cannonLeft || out.missileLeft;
	out.heavyTotal = out.cannonTotal + out.missileTotal;
	out.valid = true;
	return out;
}

inline bool ShouldRetreatForLoadoutOrAmmo(const TurretWeaponSummary& s)
{
	return s.valid && (s.mgMixed != 0 || (s.heavyPresent && !s.heavyLeft));
}
} // namespace EmpiresVehicleAim

#endif
