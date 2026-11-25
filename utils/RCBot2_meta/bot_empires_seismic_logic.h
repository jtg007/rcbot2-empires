// Pure-C++ target selection and ballistic solver for Engineer seismic grenades (TODO 5.31.1 / 5.7).
// SDK-free so it can be unit-tested offline.
#ifndef __BOT_EMPIRES_SEISMIC_LOGIC_H__
#define __BOT_EMPIRES_SEISMIC_LOGIC_H__

#include <algorithm>
#include <cmath>
#include <cstring>

namespace EmpiresSeismic
{
// Measured projectile gravity (apex/descent trace, 23.11.2026), ~0.4x sv_gravity.
constexpr float kGravity = 320.0f;
constexpr float kMinRange = 170.0f;          // blast radius 300, full damage <= 150
constexpr float kStandingMaxRange = 1550.0f; // standing flat max ~1758 HU (v 750, g 320)
constexpr float kRunningMaxRange = 2250.0f;  // running throw (+180 HU/s) reaches past MG turret 2000 HU range
constexpr float kRunSpeed = 180.0f;          // conservative forward run velocity at release
constexpr float kRunReleaseMinSpeed = 140.0f;

inline bool IsMapModelClass(const char* cls)
{
	return cls != nullptr && std::strcmp(cls, "emp_eng_map_model") == 0;
}

inline bool IsDeployableClass(const char* cls)
{
	return cls != nullptr && (std::strcmp(cls, "emp_eng_camera") == 0 ||
		std::strcmp(cls, "emp_eng_radar") == 0 ||
		std::strcmp(cls, "emp_eng_walls") == 0);
}

inline bool IsTurretClass(const char* cls)
{
	return cls != nullptr && std::strstr(cls, "turret") != nullptr;
}

// Eligible entity classes for seismic throws. Callers still verify team, health > 0,
// and for emp_eng_map_model that the obstacle is built (IsBuiltEmpiresMapObject) and not GrenadierOnly.
inline bool IsSeismicTargetClass(const char* cls)
{
	if (cls == nullptr)
		return false;
	if (std::strncmp(cls, "emp_building_", 13) == 0)
		return std::strstr(cls, "physbox") == nullptr && std::strstr(cls, "panel") == nullptr;
	return IsDeployableClass(cls) || IsMapModelClass(cls);
}

// Lower score = higher priority (added to 2D distance).
// InfantrySeismicResist: turrets -0.7, engineer camera/radar -1.0 (extra damage),
// wall 0.5 (half), large buildings 0 (full; 20 HP per hit on barracks).
inline float TargetPreference(const char* cls)
{
	if (cls == nullptr)
		return 0.0f;
	if (IsTurretClass(cls))
		return -400.0f;
	if (std::strstr(cls, "eng_camera") != nullptr || std::strstr(cls, "eng_radar") != nullptr)
		return -300.0f;
	if (IsMapModelClass(cls))
		return -150.0f;
	if (std::strstr(cls, "wall") != nullptr)
		return 250.0f;
	return 0.0f;
}

// CBaseSDKGrenade::ThrowGrenade (0x445240): view pitch p (Source, + = down) is bent to
// a = -10 + p * 10/9, speed v = min(750, (90 - a) * 6) along a, plus player forward velocity runSpeed.
// Returns the lowest view pitch whose arc reaches (dist, dz) from the eye.
inline bool SolveThrowPitch(const float dist, const float dz, const float runSpeed,
	float& outPitch, float& outElev, float& outSpeed)
{
	if (dist < 1.0f)
		return false;
	for (float p = 60.0f; p >= -80.0f; p -= 0.5f)
	{
		const float elev = -(-10.0f + p * (10.0f / 9.0f)); // degrees above horizontal
		const float v = (std::min)(750.0f, (90.0f + elev) * 6.0f);
		const float rad = elev * 3.14159265f / 180.0f;
		const float vx = v * std::cos(rad) + (std::max)(0.0f, runSpeed);
		if (vx < 1.0f)
			continue;
		const float t = dist / vx;
		const float z = v * std::sin(rad) * t - 0.5f * kGravity * t * t;
		if (z >= dz)
		{
			outPitch = p;
			outElev = elev;
			outSpeed = v;
			return true;
		}
	}
	return false;
}

struct ThrowSolution
{
	bool valid = false;
	bool running = false;
	float pitch = 0.0f;
	float elev = 0.0f;
	float speed = 0.0f;
};

// Prefers a standing throw inside kStandingMaxRange; falls back to a running throw
// up to kRunningMaxRange (e.g. tossing at a firing MG turret from > 2000 HU range).
inline ThrowSolution PlanThrow(const float landDist, const float dz)
{
	ThrowSolution sol;
	if (landDist < kMinRange || landDist > kRunningMaxRange)
		return sol;
	if (landDist <= kStandingMaxRange &&
		SolveThrowPitch(landDist, dz, 0.0f, sol.pitch, sol.elev, sol.speed))
	{
		sol.valid = true;
		sol.running = false;
		return sol;
	}
	if (SolveThrowPitch(landDist, dz, kRunSpeed, sol.pitch, sol.elev, sol.speed))
	{
		sol.valid = true;
		sol.running = true;
	}
	return sol;
}
} // namespace EmpiresSeismic

#endif
