// Pure-C++ infantry skirmish range bands, crouch cadence, navmesh hurt-cover lifecycle,
// and cover fire-permission rules (TODO 3.25).
// SDK-free so it can be unit-tested offline (scripts/run_skirmish_logic_tests.sh).
#ifndef BOT_EMPIRES_SKIRMISH_LOGIC_H
#define BOT_EMPIRES_SKIRMISH_LOGIC_H

#include <algorithm>

namespace EmpiresSkirmishLogic
{
// Empires class IDs (matching EMP_CLASS_* in bot_empires.h).
constexpr int kClassScout = 0;
constexpr int kClassRifleman = 1;
constexpr int kClassGrenadier = 2;
constexpr int kClassEngineer = 3;

// Skirmish crouch thresholds.
constexpr float kCrouchRange = 500.0f;      // never stop/crouch inside close quarters
constexpr float kLongCrouchRange = 700.0f;  // higher burst-crouch chance for long-range weapons
constexpr float kMaxLeaveDist = 2000.0f;    // cap skirmish hysteresis to avoid long-range standoffs

// Navmesh hurt-cover timings (replaces legacy 4-7 s CGotoHideSpotSched + 12 s cap so
// Tier 1/2 priority orders are not stalled).
constexpr float kHurtCoverWaitMin = 2.0f;
constexpr float kHurtCoverWaitMax = 3.5f;
constexpr float kHurtCoverMaxDuration = 6.0f;
constexpr float kSkirmishCoverMaxDuration = 3.5f;

struct SkirmishBandInput
{
	float weaponMaxRange = 0.0f;
	float weaponMinRange = 0.0f;
	bool weaponScoped = false;
	int classId = -1;
	bool enemyInVehicle = false;
	bool grenadierRpgEnabled = true;
	bool hasRocket = false;
};

struct SkirmishBandResult
{
	float nearDist = 180.0f;
	float farDist = 650.0f;
	float crouchChance = 0.20f;
	bool rocketStandoff = false;
};

// Class- and weapon-appropriate engagement band:
//   - Shotgun / close-range (<= 900):  110..380  (closes in aggressively)
//   - SMG / pistol (<= 1800):          180..650  (mid-close mobility)
//   - Rifle / 50cal / HMG (<= 3000):   300..1050 (keeps mid-range discipline)
//   - Sniper (WEAP_FL_SCOPE / min>=350): 550..1600 (real standoff + crouched bursts)
//   - Grenadier RPG vs vehicle:        550..1300
//   - Grenadier on foot:               far capped at 880 (mortar takes >= 900), near >= 400 with RPG
inline SkirmishBandResult ComputeSkirmishBand(const SkirmishBandInput& in)
{
	SkirmishBandResult out;
	const bool rocketStandoff = in.enemyInVehicle && in.classId == kClassGrenadier &&
		in.grenadierRpgEnabled && in.hasRocket;
	if (rocketStandoff)
	{
		out.nearDist = 550.0f;
		out.farDist = 1300.0f;
		out.crouchChance = 0.25f;
		out.rocketStandoff = true;
		return out;
	}

	const float maxRange = in.weaponMaxRange > 0.0f ? in.weaponMaxRange : 1200.0f;
	if (in.weaponScoped || in.weaponMinRange >= 350.0f)
	{
		out.nearDist = 550.0f;
		out.farDist = 1600.0f;
		out.crouchChance = 0.35f;
	}
	else if (maxRange <= 900.0f)
	{
		out.nearDist = 110.0f;
		out.farDist = 380.0f;
		out.crouchChance = 0.10f;
	}
	else if (maxRange <= 1800.0f)
	{
		out.nearDist = 180.0f;
		out.farDist = 650.0f;
		out.crouchChance = 0.20f;
	}
	else
	{
		out.nearDist = 300.0f;
		out.farDist = 1050.0f;
		out.crouchChance = 0.30f;
	}

	if (in.classId == kClassGrenadier)
	{
		out.farDist = (std::min)(out.farDist, 880.0f);
		if (in.grenadierRpgEnabled && in.hasRocket)
			out.nearDist = (std::max)(out.nearDist, 400.0f);
	}
	return out;
}

// Hysteresis exit distance for handleEmpiresSkirmish (bounded so long-range bots never freeze).
inline float ComputeLeaveDist(float farDist, bool skirmishActive, bool scoutFlank, bool engineerRear)
{
	float leave = skirmishActive ? farDist * 1.25f : farDist;
	leave = (std::min)(leave, kMaxLeaveDist);
	if (scoutFlank)
		leave = (std::max)(leave, 1200.0f);
	if (engineerRear)
		leave = (std::max)(leave, 1600.0f);
	return leave;
}

// Decides whether the bot pauses for a brief crouched burst instead of taking another strafe step.
// Never pauses below kCrouchRange unless blocked-step escalation (stage1Blocked) forced a crouch.
inline bool ShouldCrouchPause(bool stage1Blocked, bool strafing, float dist, int phase,
	float crouchChance, float roll01)
{
	if (stage1Blocked)
		return true;
	if (!strafing || phase != 1 || dist <= kCrouchRange)
		return false;
	const float effectiveChance = dist > kLongCrouchRange ? crouchChance : (std::min)(crouchChance, 0.20f);
	return roll01 >= 0.0f && roll01 < effectiveChance;
}

// Empires hurt-cover trigger (3.25b). The generic CBot::hurt hides only after more than
// MaxHealth x braveness (0.7 -> 70 HP) inside one 0.5 s window, which a bot in Empires hardly ever
// survives: 0 hurt covers in 10-bot matches. Here: >= 35 % of max health within 2 s, then a
// cooldown so continuous fire does not keep the bot hiding instead of shooting back.
constexpr float kHurtWindowSeconds = 2.0f;
constexpr float kHurtWindowShare = 0.35f;
constexpr float kHurtCoverCooldown = 9.0f;

struct HurtWindow
{
	float start = -1.0f;   // first damage of the current window (-1: none)
	float damage = 0.0f;
	float cooldownUntil = 0.0f;
};

// Adds one damage event; true when a hurt cover should start now.
inline bool AddHurtDamage(HurtWindow& w, float now, float damage, float maxHealth)
{
	if (w.start < 0.0f || now - w.start > kHurtWindowSeconds)
	{
		w.start = now;
		w.damage = 0.0f;
	}
	if (damage > 0.0f)
		w.damage += damage;
	if (now < w.cooldownUntil || !(maxHealth > 0.0f))
		return false;
	if (w.damage < kHurtWindowShare * maxHealth)
		return false;
	w.cooldownUntil = now + kHurtCoverCooldown;
	w.start = -1.0f;
	w.damage = 0.0f;
	return true;
}

// 3.25b: Empires sends no player_hurt (not in modevents.res), so CBot::hurt never ran. The bot's own
// health is compared once per think instead and a drop is fed into hurt().
constexpr int kPolledHurtMinDamage = 3;                 // scrapes and tiny fall damage
constexpr float kPolledHurtAttackerSeenSeconds = 1.5f;  // current enemy counts as attacker only if seen this recently
constexpr float kPolledHurtEngineerInterruptShare = 0.2f; // unseen hit must cost 20 % to stop engineer work

struct PolledHurtInput
{
	bool enabled = true;
	bool alive = true;
	bool inVehicle = false;
	bool worldDamage = false;       // m_DmgOrigin at the world origin: fall, drowning, trigger_hurt
	int lastHealth = 0;             // 0 = no reading from this life yet
	int health = 0;
	int maxHealth = 100;
	float enemySeenAgo = -1.0f;     // seconds since the current enemy was seen, < 0 = no enemy
	bool buildAssigned = false;     // director build assignment (VF, radar, ...) owns the engineer
};

struct PolledHurtDecision
{
	bool fire = false;              // call hurt()
	int damage = 0;
	bool passAttacker = false;      // hand the current enemy to hurt() as attacker, else nullptr
	bool allowCover = false;        // false -> hurt(..., bDontHide = true)
	bool interruptEngineer = false; // allow interruptEmpiresEngineerSupportForCombat
};

inline PolledHurtDecision DecidePolledHurt(const PolledHurtInput& in)
{
	PolledHurtDecision d;
	if (!in.enabled || !in.alive || in.inVehicle || in.lastHealth <= 0 || in.health <= 0 ||
		in.health >= in.lastHealth)
		return d;
	d.damage = in.lastHealth - in.health;
	if (in.worldDamage || d.damage < kPolledHurtMinDamage)
		return d;
	d.fire = true;
	d.passAttacker = in.enemySeenAgo >= 0.0f && in.enemySeenAgo <= kPolledHurtAttackerSeenSeconds;
	d.allowCover = !in.buildAssigned;
	const int maxHealth = in.maxHealth > 0 ? in.maxHealth : 100;
	d.interruptEngineer = !in.buildAssigned &&
		(d.passAttacker || static_cast<float>(d.damage) >= kPolledHurtEngineerInterruptShare * static_cast<float>(maxHealth));
	return d;
}

enum HurtCoverAction
{
	HURT_COVER_NONE = 0,
	HURT_COVER_USE_NAVMESH,
	HURT_COVER_USE_LEGACY_WAYPOINT
};

// Replaces the legacy CGotoHideSpotSched fallback in CBotEmpires::hurt when a navmesh is loaded.
inline HurtCoverAction DecideHurtCoverAction(bool navmeshHurtCoverCvar, bool navmeshLoaded,
	bool skirmishActive, bool fightCoverCvar, bool damageThresholdExceeded)
{
	if (!damageThresholdExceeded)
		return HURT_COVER_NONE;
	if (skirmishActive && fightCoverCvar)
		return HURT_COVER_NONE; // active skirmish already runs findEmpiresSkirmishCover
	if (navmeshHurtCoverCvar && navmeshLoaded)
		return HURT_COVER_USE_NAVMESH;
	return HURT_COVER_USE_LEGACY_WAYPOINT;
}

// Bounds navmesh hurt-cover duration and releases immediately for higher-priority work.
inline bool ShouldEndHurtCover(float elapsedSeconds, bool routeFinished, float waitElapsedSeconds,
	float waitDurationSeconds, bool hasHigherPriorityJob)
{
	if (hasHigherPriorityJob)
		return true;
	if (elapsedSeconds >= kHurtCoverMaxDuration)
		return true;
	if (routeFinished && waitElapsedSeconds >= waitDurationSeconds)
		return true;
	return false;
}

// Replaces unconditional cover=holding-fire in CBotEmpires::handleWeapons:
// under navmesh hurt-cover, keep firing at a visible enemy unless reloading.
inline bool ShouldHoldFireInCover(bool coverActive, bool navmeshHurtCoverCvar,
	bool enemyVisible, bool reloading)
{
	if (!coverActive)
		return false;
	if (!navmeshHurtCoverCvar)
		return true; // legacy behaviour: always hold fire during SCHED_GOOD_HIDE_SPOT
	if (reloading)
		return true;
	return !enemyVisible;
}
} // namespace EmpiresSkirmishLogic

#endif
