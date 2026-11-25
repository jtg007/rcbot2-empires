// Pure-C++ planner for Engineer refinery turret type and spot layout (TODO 5.31).
// Normal maps: 1 MG turret (anti-infantry, built first) + 1 ML turret (anti-vehicle, kit item 2).
// emp_rainbowroad: 1 MG turret + up to 4 ML turrets in a defensive line across the lane.
#ifndef __BOT_EMPIRES_TURRET_PLAN_H__
#define __BOT_EMPIRES_TURRET_PLAN_H__

#include <cmath>
#include <cstring>

namespace EmpiresTurretPlan
{
constexpr int kItemMGTurret = 1;
constexpr int kItemMLTurret = 2;

struct RefineryTurretTarget
{
	int maxMG = 1;
	int maxML = 1;
	float countRadius = 600.0f;
	bool lineFormation = false;
};

struct TurretDecision
{
	bool shouldBuild = false;
	int buildItem = kItemMGTurret; // 1 = MG, 2 = ML
	int slotIndex = 0;             // 0..4 for lineFormation; 0..1 for standard
	float yawOffsetDeg = 0.0f;     // angular offset around refinery for standard maps
	float lateralOffset = 0.0f;    // lateral offset perpendicular to frontYaw for lineFormation
};

inline bool IsRainbowroadMap(const char* mapName)
{
	if (mapName == nullptr)
		return false;
	for (const char* p = mapName; *p != '\0'; ++p)
	{
		// Case-insensitive substring check for "rainbowroad".
		static const char kNeedle[] = "rainbowroad";
		std::size_t i = 0;
		while (kNeedle[i] != '\0' && p[i] != '\0')
		{
			char c = p[i];
			if (c >= 'A' && c <= 'Z')
				c = static_cast<char>(c - 'A' + 'a');
			if (c != kNeedle[i])
				break;
			++i;
		}
		if (kNeedle[i] == '\0')
			return true;
	}
	return false;
}

inline RefineryTurretTarget TargetForMap(const char* mapName)
{
	RefineryTurretTarget target;
	if (IsRainbowroadMap(mapName))
	{
		target.maxMG = 1;
		target.maxML = 4;
		target.countRadius = 750.0f;
		target.lineFormation = true;
	}
	return target;
}

inline float AngleDiffDeg(float a, float b)
{
	float d = std::fmod(std::fabs(a - b), 360.0f);
	return d > 180.0f ? 360.0f - d : d;
}

// Decides which turret type (MG=1, ML=2) and offset to build next at a refinery.
// MG is always built first when missing so solo infantry cannot take out the refinery;
// once an MG stands, ML turrets are built to counter vehicles.
inline TurretDecision PlanNextRefineryTurret(const RefineryTurretTarget& target, int nearMG, int nearML,
	float frontYawDeg, bool haveEnemyHome, float enemyYawDeg, int attempt)
{
	TurretDecision out;
	if (nearMG < target.maxMG)
	{
		out.shouldBuild = true;
		out.buildItem = kItemMGTurret;
		out.slotIndex = 0;
	}
	else if (nearML < target.maxML)
	{
		out.shouldBuild = true;
		out.buildItem = kItemMLTurret;
		out.slotIndex = nearML + 1;
	}
	else
	{
		return out;
	}

	static const float kTurn[] = {0.0f, 45.0f, -45.0f, 90.0f, -90.0f};
	const int turnIndex = attempt >= 0 ? (attempt % 5) : 0;

	if (target.lineFormation)
	{
		// Defensive line across the lane: MG in the middle, ML turrets fanning out on both sides.
		static const float kLineOffsets[] = {0.0f, -220.0f, 220.0f, -440.0f, 440.0f};
		const int idx = (out.slotIndex + turnIndex) % 5;
		out.lateralOffset = kLineOffsets[idx];
		out.yawOffsetDeg = 0.0f;
	}
	else
	{
		if (nearMG + nearML > 0)
		{
			// Second turret (normally ML): place 70 deg off the first, preferring the sheltered side
			// covered by the refinery wall when enemy direction is known.
			float side = 70.0f;
			if (haveEnemyHome)
				side = AngleDiffDeg(frontYawDeg + 70.0f, enemyYawDeg) >= AngleDiffDeg(frontYawDeg - 70.0f, enemyYawDeg)
					? 70.0f
					: -70.0f;
			out.yawOffsetDeg = side + kTurn[turnIndex];
		}
		else
		{
			out.yawOffsetDeg = kTurn[turnIndex];
		}
	}
	return out;
}

inline const char* TurretClassForItem(int buildItem)
{
	return buildItem == kItemMLTurret ? "emp_building_mlturret" : "emp_building_mgturret";
}

inline const char* TurretPartForItem(int buildItem)
{
	return buildItem == kItemMLTurret ? "mlturret" : "mgturret";
}

inline const char* TurretLabelForItem(int buildItem)
{
	return buildItem == kItemMLTurret ? "ml" : "mg";
}
} // namespace EmpiresTurretPlan

#endif
