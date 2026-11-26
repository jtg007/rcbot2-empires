// Infantry skirmish movement (TODO 3.25): while a visible enemy soldier is inside the bot's
// effective weapon band, the bot no longer runs straight at him (APPROACH_ENEMY ended nose to nose).
// Beyond the band it keeps advancing (no long-range standoffs, user 23.11.). Inside it side-steps in
// short irregular bursts away from the enemy's aim, backs off when too close, reloading or badly
// hurt, and only rarely stops for a short crouched burst; it shoots all the time through the normal
// handleWeapons() path. Rules follow YaPB's attackMovement (strafe side from the enemy's facing,
// 30 % side swap, 0.3-0.8 s re-decision, back off under ~100 and while reloading, stand still in a
// corridor) without its long-range "stay".
//
// No new hang spots: every step target is snapped to the navmesh (no drop, no climb > 40) and
// hull-traced from the bot (walls, buildings, other players). A step without progress flips the
// side at once; repeated blocks escalate (stage 1: stand and crouch, stage 2: hand the fight back to
// the old approach for a while), each with a stall-escalate line.
#include "bot_empires.h"
#include "bot_empires_skirmish_logic.h"
#include "bot_globals.h"
#include "bot_getprop.h"
#include "bot_navmesh_api.h"
#include "bot_schedule.h"
#include "bot_buttons.h"
#include "bot_cvars.h"
#include "bot_weapons.h"
#include "bot_empires_team_director.h"
#include "iplayerinfo.h"
#include <algorithm>
#include <cmath>
#include <cstring>

int EmpiresVehiclePlayerVehicle(const CBotEmpires* bot, const edict_t* player); // bot_empires_vehicle.cpp
bool EmpiresVehicleDamageTotal(const edict_t* e, float& out);                  // bot_empires_vehicle.cpp

extern IServerGameEnts* servergameents;

namespace
{
constexpr float kStepLength = 150.0f;      // side step target distance
constexpr float kMaxStepRise = 40.0f;      // navmesh climb limit
constexpr float kMaxStepDrop = 40.0f;      // keep away from ledges entirely
constexpr float kProgressCheck = 0.35f;    // seconds into a step before judging progress
constexpr float kMinStepSpeed = 25.0f;     // HU/s below which a step counts as blocked (firing bots walk ~45)
constexpr int kStage1Blocks = 4;           // blocked steps in one fight -> stand and crouch
constexpr int kStage2Blocks = 8;           // -> give the fight back to the approach task
constexpr float kStage2Pause = 15.0f;
constexpr float kCrouchRange = EmpiresSkirmishLogic::kCrouchRange; // short crouched bursts only beyond this
constexpr float kTooClose = 110.0f;        // always back off inside this
constexpr float kSightGrace = 1.0f;        // keep moving like in the fight this long after losing sight

// TODO 8.2 self-preservation.
constexpr float kShakenSeconds = 5.0f;     // cover / fall-back time after two teammates fell next to me
constexpr float kShakenCooldown = 20.0f;   // from the start: bots must still push on afterwards
constexpr float kShakenProgress = 1.0f;    // seconds on the way before judging progress
EmpiresTeamDirector::NearDeathRing s_nearDeaths[4];

class CSkirmishTraceFilter : public CTraceFilter
{
public:
	CSkirmishTraceFilter(const IHandleEntity* self, const IHandleEntity* enemy) : m_self(self), m_enemy(enemy) {}
	bool ShouldHitEntity(IHandleEntity* entity, int) override
	{
		if (entity == m_self || entity == m_enemy)
			return false;
		// Volumes a player walks through. Bots fight on resource points, and the start-solid hit on
		// emp_resource_point rejected every step there (match 20261123-133220: 8 of 11 no-step
		// fights); same list as the navmesh stuck trace.
		IServerUnknown* unknown = static_cast<IServerUnknown*>(entity);
		CBaseEntity* base = unknown != nullptr ? unknown->GetBaseEntity() : nullptr;
		edict_t* e = base != nullptr && servergameents != nullptr ? servergameents->BaseEntityToEdict(base) : nullptr;
		const char* name = e != nullptr ? e->GetClassName() : nullptr;
		return name == nullptr || (std::strcmp(name, "emp_resource_point") != 0 &&
			std::strcmp(name, "emp_eng_restrict") != 0 && std::strcmp(name, "emp_comm_restrict") != 0 &&
			std::strcmp(name, "emp_building_vehicle_physbox") != 0);
	}
private:
	const IHandleEntity* m_self;
	const IHandleEntity* m_enemy;
};

// Preferred distance band from class and weapon (3.25, bot_empires_skirmish_logic.h): a shotgun
// closes in, a rifle keeps mid range, a sniper keeps its standoff.
EmpiresSkirmishLogic::SkirmishBandResult SkirmishBand(const CBotWeapon* weapon, int classId, bool rocketStandoff,
	bool hasRocket)
{
	EmpiresSkirmishLogic::SkirmishBandInput in;
	in.weaponMaxRange = weapon != nullptr && weapon->getWeaponInfo() != nullptr ? weapon->getPrimaryMaxRange() : 0.0f;
	in.weaponScoped = weapon != nullptr && weapon->getWeaponInfo() != nullptr && weapon->getWeaponInfo()->isScoped();
	in.classId = classId;
	in.enemyInVehicle = rocketStandoff;
	in.grenadierRpgEnabled = rcbot_empires_grenadier_rpg.GetBool();
	in.hasRocket = hasRocket;
	return EmpiresSkirmishLogic::ComputeSkirmishBand(in);
}
} // namespace

bool CBotEmpires::empiresSkirmishSpotSafe(const Vector& origin, const Vector& want, edict_t* enemy,
	Vector& out) const
{
	Vector spot = want;
	if (RCBotNavMesh::IsLoaded())
	{
		if (!RCBotNavMesh::ClosestPointOnMesh(want, 48.0f, spot) || (spot - want).Length2D() > 48.0f)
		{
			m_iSkirmishRejectMask |= 1;
			return false;
		}
		const float dz = spot.z - origin.z;
		if (dz > kMaxStepRise || dz < -kMaxStepDrop)
		{
			m_iSkirmishRejectMask |= 2;
			return false;
		}
		if (RCBotNavMesh::PointNearBuilding(spot, 24.0f))
		{
			m_iSkirmishRejectMask |= 4;
			return false;
		}
	}
	if (enginetrace == nullptr)
		return false;

	CSkirmishTraceFilter filter(m_pEdict != nullptr ? m_pEdict->GetIServerEntity() : nullptr,
		enemy != nullptr ? enemy->GetIServerEntity() : nullptr);
	trace_t hit;
	Ray_t ray;
	// Player hull lifted over steps; anything solid in between (wall, prop, building, teammate) blocks.
	ray.Init(origin + Vector(0, 0, 20), Vector(spot.x, spot.y, origin.z + 20),
		Vector(-17, -17, 0), Vector(17, 17, 50));
	enginetrace->TraceRay(ray, MASK_PLAYERSOLID, &filter, &hit);
	if (hit.startsolid || hit.fraction < 0.99f)
	{
		m_iSkirmishRejectMask |= hit.startsolid ? 32 : 8;
		if (m_szSkirmishRejectHit[0] == 0 && hit.m_pEnt != nullptr)
		{
			const edict_t* hitEdict = servergameents != nullptr ? servergameents->BaseEntityToEdict(hit.m_pEnt) : nullptr;
			snprintf(m_szSkirmishRejectHit, sizeof(m_szSkirmishRejectHit), "%s",
				hitEdict != nullptr && hitEdict->GetClassName() != nullptr ? hitEdict->GetClassName() : "?");
		}
		return false;
	}

	// Floor under the target and halfway there: no hole or ledge on the way.
	for (const float f : {0.5f, 1.0f})
	{
		const Vector p = origin + (Vector(spot.x, spot.y, origin.z) - origin) * f;
		Ray_t down;
		down.Init(p + Vector(0, 0, 16), p - Vector(0, 0, kMaxStepDrop + 16));
		trace_t floor;
		enginetrace->TraceRay(down, MASK_PLAYERSOLID_BRUSHONLY, &filter, &floor);
		if (floor.fraction >= 1.0f || floor.startsolid)
		{
			m_iSkirmishRejectMask |= 16;
			return false;
		}
	}
	out = spot;
	return true;
}

void CBotEmpires::takeEmpiresMovement(const char* why)
{
	if (m_bEmpiresApproachTaskIssued || !m_pSchedules->isEmpty() || m_navigationIntent.active)
	{
		m_bEmpiresApproachTaskIssued = false;
		m_pEmpiresApproachTarget = nullptr;
		cancelNavigationAttempt(why);
		// A cancelled provider can hand the intent to a fallback provider that keeps owning the
		// movement and silently rejects setMoveTo; end the intent outright.
		if (m_navigationIntent.active)
			finishNavigationIntent(RCBotNavigation::Result::CANCELLED, why);
		m_pSchedules->freeMemory();
		if (m_pNavigator != nullptr)
			m_pNavigator->clear();
	}
}

void CBotEmpires::endEmpiresSkirmish(const char* reason)
{
	if (!m_bSkirmishActive)
		return;
	const float now = gpGlobals != nullptr ? gpGlobals->curtime : 0.0f;
	RCBOT_DIAG("[RCBOT2] skirmish end userid=%d reason=%s secs=%.1f steps=%d blocked=%d pauses=%d covers=%d odds_back=%d flanks=%d rear=%d class_mode=%d\n",
		engine != nullptr ? engine->GetPlayerUserId(m_pEdict) : -1, reason, now - m_flSkirmishStarted,
		m_iSkirmishSteps, m_iSkirmishBlocks, m_iSkirmishPauses, m_iSkirmishCovers, m_iSkirmishOddsBack,
		m_iSkirmishFlanks, m_iSkirmishRearHolds, m_iSkirmishClassMode);
	m_bSkirmishActive = false;
	m_pSkirmishEnemy = nullptr;
	m_iSkirmishPhase = 0;
	const int saved = m_iMoveLookPriority;
	setMoveLookPriority(MOVELOOK_OVERRIDE);
	stopMoving();
	setMoveLookPriority(saved);
}

bool CBotEmpires::handleEmpiresSkirmish()
{
	// 1 = all bots, 2 = NF only, 3 = BE only (side-by-side comparison against the old approach).
	const int skirmishMode = rcbot_empires_skirmish.GetInt();
	if (skirmishMode <= 0 || (skirmishMode >= 2 && skirmishMode != getTeam()) || gpGlobals == nullptr || engine == nullptr ||
		m_pSchedules == nullptr || m_pPlayerInfo == nullptr)
	{
		endEmpiresSkirmish("off");
		return false;
	}
	const float now = gpGlobals->curtime;
	trackEmpiresRocketShot(now);
	// Not run for a moment (vehicle, commander, action took the bot): the fight state is stale.
	if (m_bSkirmishActive && now - m_flSkirmishTick > 0.5f)
		endEmpiresSkirmish("not-ticked");
	m_flSkirmishTick = now;
	edict_t* enemy = m_pEnemy.get();
	const bool seen = enemy != nullptr && hasSomeConditions(CONDITION_SEE_CUR_ENEMY) &&
		!hasSomeConditions(CONDITION_ENEMY_DEAD) && CBotGlobals::entityIsValid(enemy) &&
		CBotGlobals::entityIsAlive(enemy) && isEnemy(enemy, false) && isVisible(enemy);
	// Sight flickers for a moment during every side-step (run 20261123-130203: fights ended after
	// 0.4-1.5 s with enemy-lost). Keep the fight movement for a short grace while he is alive.
	if (seen)
		m_flSkirmishLastSeen = now;
	else if (m_bSkirmishActive && (now - m_flSkirmishLastSeen < kSightGrace ||
		(m_iSkirmishPhase >= 4 && now < m_flSkirmishPhaseUntil)))
	{
		edict_t* held = m_pSkirmishEnemy.get();
		if (held != nullptr && CBotGlobals::entityIsValid(held) && CBotGlobals::entityIsAlive(held) &&
			isEnemy(held, false))
			enemy = held;
	}
	const bool valid = seen || (enemy != nullptr && enemy == m_pSkirmishEnemy.get() && m_bSkirmishActive);
	// Only soldiers on foot: vehicles and buildings keep their own handling.
	const int enemyIndex = valid ? engine->IndexOfEdict(enemy) : 0;
	const bool enemyPlayer = valid && enemyIndex > 0 && enemyIndex <= gpGlobals->maxClients;
	const bool inVehicle = enemyPlayer && EmpiresVehiclePlayerVehicle(this, enemy) >= 0;
	int classId = -1;
	{
		bool desired = false, desiredKnown = false;
		const char* prop = nullptr;
		if (!readEmpiresClass(m_pEdict, classId, desired, desiredKnown, prop))
			classId = -1;
	}
	// Step 6: a grenadier with rockets keeps a stand-off distance to a vehicle and fires the RPG
	// from there (getBestWeapon picks it from 300 HU on) instead of running at it.
	const bool rocketStandoff = inVehicle && classId == EMP_CLASS_GRENADIER &&
		rcbot_empires_grenadier_rpg.GetBool() && empiresHasRocket();
	if (!enemyPlayer || (inVehicle && !rocketStandoff))
	{
		endEmpiresSkirmish(valid ? "not-soldier" : "enemy-lost");
		return false;
	}
	if (now < m_flSkirmishDisabledUntil || EmpiresIsEngibot(m_pEdict) || m_iAmmoCrateStage != 0 ||
		m_iEmpiresBuildingActionMode != 0 || m_iReviveTarget != 0 || m_iHealTarget != 0 ||
		lawlietBlocksWeapons())
	{
		endEmpiresSkirmish("busy");
		return false;
	}
	// A bot still walking to its capture keeps the existing relevance gate.
	if (m_bEmpiresCaptureApproachIssued && !m_bEmpiresApproachTaskIssued &&
		isEmpiresCaptureApproachPointStillValid())
	{
		endEmpiresSkirmish("capture");
		return false;
	}

	const Vector origin = getOrigin();
	Vector toEnemy = CBotGlobals::entityOrigin(enemy) - origin;
	toEnemy.z = 0.0f;
	const float dist = toEnemy.Length2D();
	if (dist < 1.0f)
	{
		endEmpiresSkirmish("overlap");
		return false;
	}
	toEnemy /= dist;

	// Grenadiers: far capped at 880 (the mortar takes 900+), near >= 400 with rockets (splash);
	// rocket standoff against a vehicle 550..1300.
	const EmpiresSkirmishLogic::SkirmishBandResult band = SkirmishBand(getCurrentWeapon(), classId, rocketStandoff,
		classId == EMP_CLASS_GRENADIER && empiresHasRocket());
	const float nearDist = band.nearDist;
	const float farDist = band.farDist;
	// Step 6: scouts circle to the other side of an enemy a teammate fights; engineers stay
	// behind that teammate. Both need a teammate closer to the enemy who can see him.
	const Vector enemyPos = CBotGlobals::entityOrigin(enemy);
	Vector allyPos;
	float allyDist = 0.0f;
	const bool scoutFlank = !rocketStandoff && classId == EMP_CLASS_SCOUT &&
		rcbot_empires_scout_flank.GetBool() && findEmpiresFrontAlly(enemy, enemyPos, allyPos, allyDist);
	const bool engineerRear = !rocketStandoff && classId == EMP_CLASS_ENGINEER &&
		rcbot_empires_engineer_rear.GetBool() && findEmpiresFrontAlly(enemy, enemyPos, allyPos, allyDist) &&
		allyDist < dist + 300.0f;
	// Hysteresis: once skirmishing, the approach takes over only well outside the band.
	const float leaveDist = EmpiresSkirmishLogic::ComputeLeaveDist(farDist, m_bSkirmishActive, scoutFlank, engineerRear);
	if (dist > leaveDist)
	{
		endEmpiresSkirmish("enemy-far");
		return false;
	}

	if (!m_bSkirmishActive || m_pSkirmishEnemy.get() != enemy)
	{
		if (m_bSkirmishActive)
			endEmpiresSkirmish("new-enemy");
		m_bSkirmishActive = true;
		m_pSkirmishEnemy = enemy;
		m_flSkirmishStarted = now;
		m_flSkirmishPhaseUntil = 0.0f;
		m_iSkirmishPhase = 0;
		m_iSkirmishSteps = m_iSkirmishBlocks = m_iSkirmishPauses = 0;
		m_iSkirmishCovers = m_iSkirmishOddsBack = 0;
		m_iSkirmishFlanks = m_iSkirmishRearHolds = 0;
		m_iSkirmishClassMode = 0;
		m_bSkirmishNoStepLogged = false;
		m_flSkirmishOddsAt = 0.0f;
		m_iSkirmishStage = 0;
		m_iSkirmishSide = randomInt(0, 1) == 0 ? -1 : 1;
		RCBOT_DIAG("[RCBOT2] skirmish start userid=%d enemy_userid=%d dist=%.0f band=%.0f-%.0f class=%d vehicle=%d flank=%d rear=%d\n",
			engine->GetPlayerUserId(m_pEdict), engine->GetPlayerUserId(enemy), dist, nearDist, farDist,
			classId, inVehicle ? 1 : 0, scoutFlank ? 1 : 0, engineerRear ? 1 : 0);
		calloutEmpiresEnemy(enemy);
	}
	// Helpers with rifles are no use against a vehicle.
	if (seen && !inVehicle)
		postEmpiresContact(enemyPos);
	// Take the movement over from the approach route and any navigation intent, once.
	takeEmpiresMovement("skirmish");

	const Vector side(-toEnemy.y, toEnemy.x, 0.0f);
	const int userId = engine->GetPlayerUserId(m_pEdict);

	// Progress: a step that does not move flips the side at once and counts as a block.
	if ((m_iSkirmishPhase == 1 || m_iSkirmishPhase == 4) && now - m_flSkirmishPhaseStarted > kProgressCheck &&
		m_vVelocity.Length2D() < kMinStepSpeed)
	{
		++m_iSkirmishBlocks;
		RCBOT_DIAG("[RCBOT2] skirmish blocked userid=%d at=(%.0f %.0f %.0f) goal=(%.0f %.0f %.0f) vel=%.0f move_valid=%d nav_owned=%d nav_intent=%d ducking=%d\n",
			userId, origin.x, origin.y, origin.z, m_vSkirmishGoal.x, m_vSkirmishGoal.y, m_vSkirmishGoal.z,
			m_vVelocity.Length2D(), moveToIsValid() ? 1 : 0, m_navigationMovementOwned ? 1 : 0,
			m_navigationIntent.active ? 1 : 0, (CClassInterface::getFlags(m_pEdict) & FL_DUCKING) ? 1 : 0);
		m_iSkirmishSide = -m_iSkirmishSide;
		m_flSkirmishPhaseUntil = 0.0f;
		if (m_iSkirmishBlocks == kStage1Blocks && m_iSkirmishStage < 1)
		{
			m_iSkirmishStage = 1;
			RCBOT_DIAG("[RCBOT2] stall-escalate who=skirmish stage=1 reason=steps-blocked userid=%d blocked=%d at=(%.0f %.0f %.0f)\n",
				userId, m_iSkirmishBlocks, origin.x, origin.y, origin.z);
		}
		else if (m_iSkirmishBlocks >= kStage2Blocks)
		{
			RCBOT_DIAG("[RCBOT2] stall-escalate who=skirmish stage=2 reason=steps-blocked userid=%d blocked=%d at=(%.0f %.0f %.0f)\n",
				userId, m_iSkirmishBlocks, origin.x, origin.y, origin.z);
			m_flSkirmishDisabledUntil = now + kStage2Pause;
			endEmpiresSkirmish("blocked");
			return false;
		}
	}

	if (m_iSkirmishPhase >= 4 && now < m_flSkirmishPhaseUntil && now - m_flSkirmishPhaseStarted > 1.0f)
	{
		const CBotWeapon* held = getCurrentWeapon();
		if (!(held != nullptr && held->clipEmptyWithReserve(this)) && getHealthPercent() >= 0.35f &&
			!(m_iSkirmishEnemies >= m_iSkirmishAllies + 2) && !empiresShaken(now))
			m_flSkirmishPhaseUntil = now;
	}
	if (now >= m_flSkirmishPhaseUntil)
	{
		const CBotWeapon* weapon = getCurrentWeapon();
		const bool reloading = weapon != nullptr && weapon->clipEmptyWithReserve(this);
		const bool hurtBadly = getHealthPercent() < 0.35f;
		// Step 2: weigh the fight. Outnumbered -> give ground; ahead -> press in.
		int allies = 0, enemies = 1;
		const bool useOdds = rcbot_empires_fight_odds.GetBool();
		if (useOdds)
		{
			if (now >= m_flSkirmishOddsAt)
			{
				countEmpiresFightOdds(origin, m_iSkirmishAllies, m_iSkirmishEnemies);
				m_flSkirmishOddsAt = now + 0.5f;
			}
			allies = m_iSkirmishAllies;
			enemies = (std::max)(1, m_iSkirmishEnemies);
		}
		// TODO 8.2: two teammates just fell next to me -> give ground and break the sight like outnumbered.
		const bool shaken = empiresShaken(now);
		const bool outnumbered = (useOdds && enemies >= allies + 2) || shaken;
		const bool ahead = useOdds && allies >= enemies;
		const char* mode = "strafe";
		if (dist < nearDist || dist < kTooClose || reloading || hurtBadly || outnumbered)
		{
			mode = "back";
			if (outnumbered && !(dist < nearDist || reloading || hurtBadly))
				++m_iSkirmishOddsBack;
		}
		else if (dist > (ahead ? nearDist * 1.5f : farDist * 0.8f))
			mode = "advance";

		// Side away from the enemy's aim (YaPB): stay on the side of his facing line we are on,
		// with a 30 % swap so the pattern does not become predictable.
		IPlayerInfo* enemyInfo = playerinfomanager != nullptr ? playerinfomanager->GetPlayerInfo(enemy) : nullptr;
		if (enemyInfo != nullptr)
		{
			Vector fwd, right;
			AngleVectors(enemyInfo->GetAbsAngles(), &fwd, &right, nullptr);
			const float onRight = (origin - enemyPos).Dot(right);
			m_iSkirmishSide = side.Dot(right) * onRight >= 0.0f ? 1 : -1;
		}
		if (randomFloat(0.0f, 1.0f) < 0.3f)
			m_iSkirmishSide = -m_iSkirmishSide;

		// Step 6 class roles.
		m_iSkirmishClassMode = rocketStandoff ? 2 : 0;
		if (scoutFlank && mode[0] != 'b')
		{
			// Circle away from the teammate (seen from the enemy) until about 100 degrees apart,
			// so the enemy cannot watch both. Moving along +side turns us clockwise around him.
			Vector allyRel = allyPos - enemyPos;
			Vector mineRel = origin - enemyPos;
			allyRel.z = mineRel.z = 0.0f;
			const float al = allyRel.Length2D();
			const float ml = mineRel.Length2D();
			const float cosApart = al > 1.0f && ml > 1.0f ? allyRel.Dot(mineRel) / (al * ml) : 1.0f;
			if (cosApart > -0.17f)
			{
				const float cross = allyRel.x * mineRel.y - allyRel.y * mineRel.x;
				m_iSkirmishSide = cross > 0.0f ? -1 : 1;
				mode = "flank";
				m_iSkirmishClassMode = 1;
				++m_iSkirmishFlanks;
			}
		}
		else if (engineerRear)
		{
			// Never in front of the teammate: at least 150 behind him, no advancing.
			if (dist < allyDist + 150.0f)
				mode = "back";
			else if (mode[0] == 'a')
				mode = "strafe";
			m_iSkirmishClassMode = 3;
			++m_iSkirmishRearHolds;
		}

		bool pause = EmpiresSkirmishLogic::ShouldCrouchPause(m_iSkirmishStage >= 1, mode[0] == 's', dist,
			m_iSkirmishPhase, band.crouchChance, randomFloat(0.0f, 1.0f));

		Vector target;
		bool found = false;
		bool coverChosen = false;
		m_iSkirmishRejectMask = 0;
		m_szSkirmishRejectHit[0] = 0;
		// Step 3: reloading, badly hurt or outnumbered -> break the line of sight if a safe spot
		// is close, crouch there until reloaded (max 3.5 s), then the fight resumes or the
		// last-known search peeks out.
		if (!pause && mode[0] == 'b' && (reloading || hurtBadly || outnumbered) &&
			rcbot_empires_fight_cover.GetBool() && findEmpiresSkirmishCover(origin, enemy, toEnemy, target))
		{
			m_iSkirmishPhase = 4;
			m_vSkirmishGoal = target;
			++m_iSkirmishCovers;
			m_flSkirmishPhaseStarted = now;
			m_flSkirmishPhaseUntil = now + 3.5f;
			RCBOT_DIAG("[RCBOT2] skirmish cover userid=%d reason=%s at=(%.0f %.0f %.0f) spot=(%.0f %.0f %.0f) allies=%d enemies=%d\n",
				userId, reloading ? "reload" : hurtBadly ? "hurt" : shaken ? "shaken" : "outnumbered", origin.x, origin.y, origin.z,
				target.x, target.y, target.z, allies, enemies);
			found = true;
			coverChosen = true;
			pause = true; // skip the step search below
		}
		for (int attempt = 0; attempt < 3 && !pause && !found; ++attempt)
		{
			const float s = static_cast<float>(attempt == 1 ? -m_iSkirmishSide : m_iSkirmishSide);
			Vector dir;
			if (mode[0] == 'b')
				dir = attempt < 2 ? side * s * 0.5f - toEnemy : side * static_cast<float>(m_iSkirmishSide);
			else if (mode[0] == 'a')
				dir = attempt < 2 ? side * s * 0.7f + toEnemy : toEnemy;
			else if (mode[0] == 'f')
				dir = attempt < 2 ? side * s + toEnemy * (dist > (nearDist + farDist) * 0.5f ? 0.5f : -0.2f) : toEnemy;
			else
				dir = attempt < 2 ? side * s : (dist < (nearDist + farDist) * 0.5f ? -toEnemy : toEnemy);
			dir.NormalizeInPlace();
			if (empiresSkirmishSpotSafe(origin, origin + dir * kStepLength, enemy, target))
			{
				found = true;
				if (attempt == 1)
					m_iSkirmishSide = -m_iSkirmishSide;
			}
		}
		if (coverChosen)
		{
			// cover phase set above
		}
		else if (found)
		{
			m_flSkirmishPhaseStarted = now;
			m_iSkirmishPhase = 1;
			m_vSkirmishGoal = target;
			++m_iSkirmishSteps;
			m_flSkirmishPhaseUntil = now + randomFloat(0.35f, 0.8f);
		}
		else
		{
			// Planned crouch burst, or no safe step at all (corridor, ledge): stand and shoot.
			if (!pause && !m_bSkirmishNoStepLogged)
			{
				m_bSkirmishNoStepLogged = true;
				RCBOT_DIAG("[RCBOT2] skirmish no-step userid=%d mode=%s at=(%.0f %.0f %.0f) dist=%.0f reject=%s%s%s%s%s%s hit=%s\n",
					userId, mode, origin.x, origin.y, origin.z, dist,
					(m_iSkirmishRejectMask & 1) ? "mesh," : "", (m_iSkirmishRejectMask & 2) ? "height," : "",
					(m_iSkirmishRejectMask & 4) ? "building," : "", (m_iSkirmishRejectMask & 8) ? "hull," : "",
					(m_iSkirmishRejectMask & 32) ? "startsolid," : "", (m_iSkirmishRejectMask & 16) ? "floor," : "",
					m_szSkirmishRejectHit[0] != 0 ? m_szSkirmishRejectHit : "-");
			}
			m_flSkirmishPhaseStarted = now;
			m_iSkirmishPhase = 2;
			++m_iSkirmishPauses;
			m_flSkirmishPhaseUntil = now + (pause ? randomFloat(0.4f, 0.7f) : randomFloat(0.8f, 1.2f));
		}
	}

	const int saved = m_iMoveLookPriority;
	setMoveLookPriority(MOVELOOK_OVERRIDE);
	if (m_iSkirmishPhase == 1 || m_iSkirmishPhase == 4)
	{
		// Arrived early: stand until the phase ends instead of jittering on the spot.
		if ((m_vSkirmishGoal - origin).Length2D() < 24.0f)
		{
			stopMoving();
			m_iSkirmishPhase = m_iSkirmishPhase == 4 ? 5 : 3;
		}
		else
			setMoveTo(m_vSkirmishGoal);
	}
	else if (m_iSkirmishPhase == 5)
	{
		stopMoving();
		if (m_pButtons != nullptr)
			m_pButtons->duck(0.1f);
	}
	else
	{
		stopMoving();
		if (m_iSkirmishPhase == 2 && dist > kCrouchRange && m_pButtons != nullptr)
			m_pButtons->duck(0.1f);
	}
	setLookAtTask(LOOK_ENEMY);
	setMoveLookPriority(saved);
	return true;
}

// ---------------------------------------------------------------------------------------------
// Step 2: own soldiers near me vs. enemy soldiers I can see.
void CBotEmpires::countEmpiresFightOdds(const Vector& origin, int& allies, int& enemies)
{
	allies = 0;
	enemies = 0;
	if (gpGlobals == nullptr || engine == nullptr || playerinfomanager == nullptr)
		return;
	const int team = getTeam();
	for (int i = 1; i <= gpGlobals->maxClients; ++i)
	{
		edict_t* player = INDEXENT(i);
		if (player == nullptr || player->IsFree() || player == m_pEdict)
			continue;
		IPlayerInfo* info = playerinfomanager->GetPlayerInfo(player);
		if (info == nullptr || !info->IsConnected() || info->IsDead() || info->GetHealth() <= 0 ||
			info->IsObserver())
			continue;
		const float d = (CBotGlobals::entityOrigin(player) - origin).Length2D();
		if (info->GetTeamIndex() == team)
		{
			if (d < 1000.0f && EmpiresVehiclePlayerVehicle(this, player) < 0)
				++allies;
		}
		else if (info->GetTeamIndex() >= 2 && d < 1500.0f && isVisible(player) &&
			EmpiresVehiclePlayerVehicle(this, player) < 0)
			++enemies;
	}
}

// Step 3: a close, safely reachable spot the enemy cannot see.
bool CBotEmpires::findEmpiresSkirmishCover(const Vector& origin, edict_t* enemy, const Vector& toEnemy,
	Vector& out) const
{
	const Vector eye = CBotGlobals::entityOrigin(enemy) + Vector(0, 0, 60);
	const Vector away = -toEnemy;
	const Vector side(-toEnemy.y, toEnemy.x, 0.0f);
	const float sideSign = static_cast<float>(m_iSkirmishSide);
	const Vector dirs[] = {
		side * sideSign, side * -sideSign,
		(away + side * sideSign) * 0.7071f, (away - side * sideSign) * 0.7071f, away };
	for (const float range : {140.0f, 220.0f, 300.0f})
	{
		for (const Vector& dir : dirs)
		{
			Vector spot;
			if (!empiresSkirmishSpotSafe(origin, origin + dir * range, enemy, spot))
				continue;
			// Hidden at standing chest height from the enemy's eyes.
			if (CBotGlobals::isVisible(enemy, eye, spot + Vector(0, 0, 48)))
				continue;
			out = spot;
			return true;
		}
	}
	return false;
}

// ---------------------------------------------------------------------------------------------
// Step 5: call the enemy out like a player would (Empires spot quick command: minimap mark and
// voice), only for a group or a close enemy, at most once per team per minute and per bot every 3 min.
namespace
{
float s_nextTeamCallout[4] = {};
float s_nextTeamBinocular[4] = {};
}

void CBotEmpires::calloutEmpiresEnemy(edict_t* enemy)
{
	if (!rcbot_empires_callouts.GetBool() || helpers == nullptr || gpGlobals == nullptr || enemy == nullptr)
		return;
	const int team = getTeam();
	const float now = gpGlobals->curtime;
	if (team < 0 || team >= 4 || now < s_nextTeamCallout[team] || now < m_flNextCallout)
		return;
	bool alreadySpotted = false;
	if (readEmpiresEntityBoolProperty(enemy, "m_bSpotted", alreadySpotted) && alreadySpotted)
		return;
	// Only calls that matter (user 23.11.: constant callouts get annoying): a group of enemies,
	// or one that is close to us; otherwise stay quiet.
	int allies = 0, enemies = 0;
	countEmpiresFightOdds(getOrigin(), allies, enemies);
	if (enemies < 2 && distanceFrom(enemy) > 800.0f)
		return;
	s_nextTeamCallout[team] = now + 60.0f;
	m_flNextCallout = now + 180.0f;
	helpers->ClientCommand(m_pEdict, "emp_menu_quickcmd 2 0\n");
	RCBOT_DIAG("[RCBOT2] callout userid=%d kind=enemy-spotted enemy_userid=%d dist=%.0f\n",
		engine->GetPlayerUserId(m_pEdict), engine->GetPlayerUserId(enemy), distanceFrom(enemy));
}

// ---------------------------------------------------------------------------------------------
// Step 1: a team-wide board of current fights. A bot in a fight posts its enemy's position; free
// infantry nearby walks there instead of to its own far goal (max 2 helpers per contact).
namespace
{
struct FightContact
{
	Vector pos;
	float updated = -1000.0f;
	int helpers = 0;
	int id = 0;
};
constexpr int kContacts = 6;
constexpr float kContactTtl = 15.0f;
constexpr float kJoinRadius = 2000.0f;
constexpr float kJoinArrive = 450.0f;
constexpr int kMaxHelpers = 2;
FightContact s_contacts[4][kContacts];
int s_nextContactId = 1;
} // namespace

void CBotEmpires::postEmpiresContact(const Vector& enemyPos)
{
	if (!rcbot_empires_join_fight.GetBool() || gpGlobals == nullptr)
		return;
	const int team = getTeam();
	const float now = gpGlobals->curtime;
	if (team < 0 || team >= 4 || now < m_flNextContactPost)
		return;
	m_flNextContactPost = now + 1.0f;
	FightContact* oldest = &s_contacts[team][0];
	for (FightContact& c : s_contacts[team])
	{
		if (now - c.updated < kContactTtl && (c.pos - enemyPos).Length2D() < 700.0f)
		{
			c.pos = enemyPos;
			c.updated = now;
			return;
		}
		if (c.updated < oldest->updated)
			oldest = &c;
	}
	oldest->pos = enemyPos;
	oldest->updated = now;
	oldest->helpers = 0;
	oldest->id = s_nextContactId++;
}

void CBotEmpires::endEmpiresJoinFight(const char* reason)
{
	if (m_iJoinContactId == 0)
		return;
	const int team = getTeam();
	if (team >= 0 && team < 4)
	{
		for (FightContact& c : s_contacts[team])
		{
			if (c.id == m_iJoinContactId && c.helpers > 0)
				--c.helpers;
		}
	}
	const float now = gpGlobals != nullptr ? gpGlobals->curtime : 0.0f;
	RCBOT_DIAG("[RCBOT2] join-fight end userid=%d contact=%d reason=%s secs=%.1f dist=%.0f\n",
		engine != nullptr ? engine->GetPlayerUserId(m_pEdict) : -1, m_iJoinContactId, reason,
		now - m_flJoinStarted, (m_vJoinGoal - getOrigin()).Length2D());
	m_iJoinContactId = 0;
	cancelNavigationAttempt("join-fight-end");
}

bool CBotEmpires::handleEmpiresJoinFight()
{
	if (!rcbot_empires_join_fight.GetBool() || gpGlobals == nullptr || engine == nullptr ||
		m_pSchedules == nullptr)
	{
		endEmpiresJoinFight("off");
		return false;
	}
	const float now = gpGlobals->curtime;
	const int team = getTeam();
	if (team < 0 || team >= 4)
		return false;
	// Engineers keep building/healing; capturers keep their flag; EngiBots stay home.
	if (getDesiredClass() == EMP_CLASS_ENGINEER || EmpiresIsEngibot(m_pEdict) ||
		m_bEmpiresCaptureApproachIssued)
	{
		endEmpiresJoinFight("busy");
		return false;
	}
	const Vector origin = getOrigin();
	const int userId = engine->GetPlayerUserId(m_pEdict);

	if (m_iJoinContactId != 0)
	{
		FightContact* contact = nullptr;
		for (FightContact& c : s_contacts[team])
			if (c.id == m_iJoinContactId)
				contact = &c;
		if (contact == nullptr || now - contact->updated > kContactTtl)
		{
			endEmpiresJoinFight("contact-over");
			return false;
		}
		const float dist = (contact->pos - origin).Length2D();
		if (dist < kJoinArrive)
		{
			endEmpiresJoinFight("arrived");
			return false;
		}
		if (dist < m_flJoinBestDist - 100.0f)
		{
			m_flJoinBestDist = dist;
			m_flJoinProgressAt = now;
		}
		else if (now - m_flJoinProgressAt > 8.0f)
		{
			RCBOT_DIAG("[RCBOT2] stall-escalate who=join-fight stage=1 reason=no-progress userid=%d contact=%d dist=%.0f\n",
				userId, m_iJoinContactId, dist);
			m_iJoinSkipId = m_iJoinContactId;
			endEmpiresJoinFight("no-progress");
			return false;
		}
		// Replan when the fight moved or the route ended.
		const bool idle = m_pSchedules->isEmpty() && !m_navigationIntent.active;
		if (now >= m_flJoinNextNav && (idle || (contact->pos - m_vJoinGoal).Length2D() > 400.0f))
		{
			m_flJoinNextNav = now + 2.0f;
			m_vJoinGoal = contact->pos;
			cancelNavigationAttempt("join-fight");
			m_pSchedules->freeMemory();
			if (!requestNavigation(m_vJoinGoal, RCBotNavigation::NavigationPolicy::NAVMESH_ONLY, 0))
			{
				m_iJoinSkipId = m_iJoinContactId;
				endEmpiresJoinFight("no-route");
				return false;
			}
		}
		return true;
	}

	if (now < m_flJoinNextSearch)
		return false;
	m_flJoinNextSearch = now + 1.0f;
	FightContact* best = nullptr;
	float bestDist = kJoinRadius;
	for (FightContact& c : s_contacts[team])
	{
		if (now - c.updated > 6.0f || c.helpers >= kMaxHelpers || c.id == m_iJoinSkipId)
			continue;
		const float d = (c.pos - origin).Length2D();
		if (d > kJoinArrive * 1.5f && d < bestDist)
		{
			best = &c;
			bestDist = d;
		}
	}
	if (best == nullptr)
		return false;
	if (m_bEmpiresNavGoalActive)
		endEmpiresNavmeshGoal("drop", "join-fight", 0.0f);
	cancelNavigationAttempt("join-fight");
	m_pSchedules->freeMemory();
	if (!requestNavigation(best->pos, RCBotNavigation::NavigationPolicy::NAVMESH_ONLY, 0))
	{
		m_iJoinSkipId = best->id;
		return false;
	}
	++best->helpers;
	m_iJoinContactId = best->id;
	m_vJoinGoal = best->pos;
	m_flJoinStarted = m_flJoinProgressAt = now;
	m_flJoinBestDist = bestDist;
	m_flJoinNextNav = now + 2.0f;
	RCBOT_DIAG("[RCBOT2] join-fight start userid=%d contact=%d dist=%.0f helpers=%d at=(%.0f %.0f %.0f)\n",
		userId, best->id, bestDist, best->helpers, best->pos.x, best->pos.y, best->pos.z);
	return true;
}

void CBotEmpires::resetEmpiresSkirmishState(bool mapChange)
{
	if (mapChange)
	{
		// Times and positions belong to the old map; curtime starts again at 0.
		for (auto& team : s_contacts)
			for (FightContact& c : team)
				c = FightContact();
		for (float& t : s_nextTeamCallout)
			t = 0.0f;
		for (float& t : s_nextTeamBinocular)
			t = 0.0f;
		for (EmpiresTeamDirector::NearDeathRing& r : s_nearDeaths)
			r = EmpiresTeamDirector::NearDeathRing();
	}
	m_bShakenActive = false;
	m_flShakenUntil = 0.0f;
	m_flShakenNext = 0.0f;
	m_flShakenAfter = -1.0e9f;
	m_bSkirmishActive = false;
	m_pSkirmishEnemy = nullptr;
	m_iSkirmishPhase = 0;
	m_flSkirmishDisabledUntil = 0.0f;
	m_flSkirmishTick = 0.0f;
	m_flSkirmishOddsAt = 0.0f;
	m_flNextCallout = 0.0f;
	m_flNextBinocularSpot = 0.0f;
	m_flNextContactPost = 0.0f;
	m_iJoinContactId = 0;
	m_iJoinSkipId = 0;
	m_flJoinNextNav = 0.0f;
	m_flJoinNextSearch = 0.0f;
}

// ---------------------------------------------------------------------------------------------
// Step 6: class roles in a fight.

// The teammate on foot closest to the enemy (within 1500) who can see him.
bool CBotEmpires::findEmpiresFrontAlly(edict_t* enemy, const Vector& enemyPos, Vector& allyPos, float& allyDist) const
{
	if (gpGlobals == nullptr || playerinfomanager == nullptr)
		return false;
	const int team = m_pPlayerInfo != nullptr ? m_pPlayerInfo->GetTeamIndex() : -1;
	bool found = false;
	allyDist = 1500.0f;
	for (int i = 1; i <= gpGlobals->maxClients; ++i)
	{
		edict_t* player = INDEXENT(i);
		if (player == nullptr || player->IsFree() || player == m_pEdict)
			continue;
		IPlayerInfo* info = playerinfomanager->GetPlayerInfo(player);
		if (info == nullptr || !info->IsConnected() || info->IsDead() || info->GetHealth() <= 0 ||
			info->IsObserver() || info->GetTeamIndex() != team)
			continue;
		const Vector pos = CBotGlobals::entityOrigin(player);
		const float d = (pos - enemyPos).Length2D();
		if (d >= allyDist || EmpiresVehiclePlayerVehicle(this, player) >= 0)
			continue;
		if (enemy != nullptr && !CBotGlobals::isVisible(player, pos + Vector(0, 0, 64), enemy))
			continue;
		allyDist = d;
		allyPos = pos;
		found = true;
	}
	return found;
}

void CBotEmpires::trackEmpiresRocketShot(float now)
{
	if (m_iRpgShotVehicle > 0 && now >= m_flRpgShotCheck)
	{
		edict_t* v = INDEXENT(m_iRpgShotVehicle);
		float after = -1.0f;
		const bool read = v != nullptr && !v->IsFree() && EmpiresVehicleDamageTotal(v, after);
		RCBOT_DIAG("[RCBOT2] rpg-shot userid=%d vehicle=%d dist=%.0f before=%.0f after=%.0f gone=%d hit=%d\n",
			engine->GetPlayerUserId(m_pEdict), m_iRpgShotVehicle, m_flRpgShotDist, m_flRpgShotBefore, after,
			read ? 0 : 1, (!read || after < m_flRpgShotBefore - 1.0f) ? 1 : 0);
		m_iRpgShotVehicle = 0;
	}
	CBotWeapon* w = getCurrentWeapon();
	if (w == nullptr || (w->getID() != 13 && w->getID() != 27))
	{
		m_iRpgLastCount = -1;
		return;
	}
	const int count = (std::max)(0, w->getClip1(this)) + (std::max)(0, w->getAmmo(this));
	const bool fired = m_iRpgLastCount >= 0 && count < m_iRpgLastCount;
	m_iRpgLastCount = count;
	if (!fired || m_iRpgShotVehicle > 0)
		return;
	edict_t* enemy = m_pEnemy.get();
	if (enemy == nullptr || !CBotGlobals::entityIsValid(enemy))
		return;
	const int enemyIndex = engine->IndexOfEdict(enemy);
	int vehicle = -1;
	if (enemyIndex > 0 && enemyIndex <= gpGlobals->maxClients)
		vehicle = EmpiresVehiclePlayerVehicle(this, enemy);
	else
		vehicle = enemyIndex;
	edict_t* v = vehicle > 0 ? INDEXENT(vehicle) : nullptr;
	float before = 0.0f;
	if (v == nullptr || !EmpiresVehicleDamageTotal(v, before))
		return;
	// Beyond the RPG range (3200) the drop is no aimed shot at it (respawn, weapon swap).
	const float dist = (CBotGlobals::entityOrigin(v) - getOrigin()).Length2D();
	if (dist > 3500.0f)
		return;
	m_iRpgShotVehicle = vehicle;
	m_flRpgShotBefore = before;
	m_flRpgShotDist = dist;
	m_flRpgShotCheck = now + 3.0f;
}

bool CBotEmpires::empiresHasRocket() const
{
	if (m_pWeapons == nullptr)
		return false;
	// weapon_emp_nf_rpg / weapon_emp_imp_rpg (EMPWeaps ids 13 and 27).
	for (const int id : {13, 27})
	{
		CWeapon* info = CWeapons::getWeapon(id);
		CBotWeapon* weapon = info != nullptr ? m_pWeapons->getWeapon(info) : nullptr;
		if (weapon != nullptr && weapon->hasWeapon() && !weapon->outOfAmmo(this))
			return true;
	}
	return false;
}

// A scout that sees an unspotted enemy far away stops and spots him with the binoculars (the
// existing select/aim/attack action), so the team sees him on the map. Per bot every 45 s, per
// team every 20 s, never with another enemy close.
bool CBotEmpires::handleEmpiresScoutBinoculars()
{
	if (isEmpiresScoutBinocularActionActive())
	{
		const int saved = m_iMoveLookPriority;
		setMoveLookPriority(MOVELOOK_OVERRIDE);
		stopMoving();
		setMoveLookPriority(saved);
		return true;
	}
	if (!rcbot_empires_scout_binoculars.GetBool() || gpGlobals == nullptr || engine == nullptr ||
		playerinfomanager == nullptr || m_bSkirmishActive || m_pPlayerInfo == nullptr)
		return false;
	const float now = gpGlobals->curtime;
	if (now < m_flNextBinocularSpot)
		return false;
	edict_t* enemy = m_pEnemy.get();
	if (enemy == nullptr || !hasSomeConditions(CONDITION_SEE_CUR_ENEMY) ||
		hasSomeConditions(CONDITION_ENEMY_DEAD) || !CBotGlobals::entityIsValid(enemy) ||
		!CBotGlobals::entityIsAlive(enemy) || !isEnemy(enemy, false) || !isVisible(enemy))
		return false;
	const int enemyIndex = engine->IndexOfEdict(enemy);
	if (enemyIndex <= 0 || enemyIndex > gpGlobals->maxClients)
		return false;
	int classId = -1;
	bool desired = false, desiredKnown = false;
	const char* prop = nullptr;
	if (!readEmpiresClass(m_pEdict, classId, desired, desiredKnown, prop) || classId != EMP_CLASS_SCOUT)
		return false;
	const Vector origin = getOrigin();
	const float dist = (CBotGlobals::entityOrigin(enemy) - origin).Length2D();
	if (dist < 1000.0f || dist > 4000.0f || getHealthPercent() < 0.5f)
		return false;
	const int team = m_pPlayerInfo->GetTeamIndex();
	if (team < 0 || team >= 4 || now < s_nextTeamBinocular[team])
		return false;
	// Already spotted (players: m_VisibilityState 3, see maybeSpotEmpiresEnemy).
	int visibilityState = 0;
	if (readEmpiresEntityIntProperty(enemy, "m_VisibilityState", visibilityState) && visibilityState == 3)
		return false;
	for (int i = 1; i <= gpGlobals->maxClients; ++i)
	{
		edict_t* player = INDEXENT(i);
		if (player == nullptr || player->IsFree() || player == enemy)
			continue;
		IPlayerInfo* info = playerinfomanager->GetPlayerInfo(player);
		if (info == nullptr || !info->IsConnected() || info->IsDead() || info->IsObserver() ||
			info->GetTeamIndex() < 2 || info->GetTeamIndex() == team)
			continue;
		if ((CBotGlobals::entityOrigin(player) - origin).Length2D() < 900.0f && isVisible(player))
			return false;
	}
	if (!requestEmpiresScoutBinocularTarget(enemy, false))
	{
		m_flNextBinocularSpot = now + 10.0f;
		return false;
	}
	m_flNextBinocularSpot = now + 45.0f;
	s_nextTeamBinocular[team] = now + 20.0f;
	RCBOT_DIAG("[RCBOT2] scout-binoculars start userid=%d target_userid=%d dist=%.0f\n",
		engine->GetPlayerUserId(m_pEdict), engine->GetPlayerUserId(enemy), dist);
	// Stand while spotting; the approach resumes afterwards.
	if (m_bEmpiresApproachTaskIssued)
	{
		m_bEmpiresApproachTaskIssued = false;
		m_pEmpiresApproachTarget = nullptr;
		if (m_pSchedules != nullptr)
			m_pSchedules->freeMemory();
		if (m_pNavigator != nullptr)
			m_pNavigator->clear();
	}
	cancelNavigationAttempt("binoculars");
	const int saved = m_iMoveLookPriority;
	setMoveLookPriority(MOVELOOK_OVERRIDE);
	stopMoving();
	setMoveLookPriority(saved);
	return true;
}

// Calm idle look. The generic LOOK_AROUND (bot.cpp) picks a random point within 128 units of
// the bot's own eye every 2-3 s: any yaw, often down or behind, so standing bots (refinery
// hold, waiting) kept whipping their view around (user 23.11.: "sinnlos hin und her schauen").
void CBotEmpires::getLookAtVector()
{
	// A walking bot (LOOK_WAYPOINT) looked 5 s toward every enemy a teammate reported, so in a
	// big match most bots ran sideways (human test 26.11.). A report still drives the approach;
	// the view only follows the bot's own sightings. Hide the report from the generic branch.
	if (m_iLookTask == LOOK_WAYPOINT && m_bEmpiresLastEnemyReported &&
		rcbot_empires_move_look_own_sight.GetBool())
	{
		const float savedSeen = m_fLastSeeEnemy;
		m_fLastSeeEnemy = -1000.0f;
		CBot::getLookAtVector();
		m_fLastSeeEnemy = savedSeen;
		return;
	}
	if (m_iLookTask != LOOK_AROUND || !rcbot_empires_calm_look.GetBool() || engine == nullptr)
	{
		CBot::getLookAtVector();
		return;
	}
	const float now = engine->Time();
	if (m_fLookAroundTime < now)
	{
		const Vector eye = getEyePosition();
		if (m_pLastEnemy.get() != nullptr && now - m_fLastSeeEnemy < 20.0f)
			m_vLookAroundOffset = Vector(m_vLastSeeEnemy.x, m_vLastSeeEnemy.y, m_vLastSeeEnemy.z + 48.0f);
		else
		{
			const QAngle level(0.0f, m_vViewAngles.y + randomFloat(-30.0f, 30.0f), 0.0f);
			Vector forward;
			AngleVectors(level, &forward);
			m_vLookAroundOffset = eye + forward * 512.0f;
		}
		m_fLookAroundTime = now + randomFloat(3.0f, 5.0f);
		++m_iCalmLookPicks;
		const Vector to = m_vLookAroundOffset - eye;
		const float turn = std::fabs(AngleNormalize(RAD2DEG(std::atan2(to.y, to.x)) - m_vViewAngles.y));
		if (m_pLastEnemy.get() != nullptr && now - m_fLastSeeEnemy < 20.0f)
			++m_iCalmLookEnemy;
		else
			m_flCalmLookMaxTurn = (std::max)(m_flCalmLookMaxTurn, turn);
		if (now >= m_flCalmLookLogAt)
		{
			RCBOT_DIAG("[RCBOT2] calm-look userid=%d picks=%d enemy=%d max_level_turn=%.0f\n",
				engine->GetPlayerUserId(m_pEdict), m_iCalmLookPicks, m_iCalmLookEnemy, m_flCalmLookMaxTurn);
			m_iCalmLookPicks = m_iCalmLookEnemy = 0;
			m_flCalmLookMaxTurn = 0.0f;
			m_flCalmLookLogAt = now + 30.0f;
		}
	}
	setLookAt(m_vLookAroundOffset);
}

// ---------------------------------------------------------------------------------------------
// TODO 8.2 self-preservation: two teammates killed by the enemy within 8 s and 600 HU of a bot make it
// take cover or fall back for a few seconds instead of walking on into the same fire. In a fight the
// skirmish treats it like being outnumbered (back off, break the sight); otherwise the bot walks
// 300-450 HU away from the killer, preferably to a spot he cannot see, and crouches there.
namespace
{
void NoteNearDeath(int team, const Vector& at, const Vector& threat, int victimUserId, int killerUserId)
{
	if (gpGlobals == nullptr || team < 2 || team > 3)
		return;
	const float a[3] = { at.x, at.y, at.z };
	const float t[3] = { threat.x, threat.y, threat.z };
	EmpiresTeamDirector::RecordNearDeath(s_nearDeaths[team], a, t, gpGlobals->curtime);
	RCBOT_DIAG("[RCBOT2] shaken death team=%d victim=%d killer=%d pos=(%.0f %.0f %.0f) threat=(%.0f %.0f %.0f)\n",
		team, victimUserId, killerUserId, at.x, at.y, at.z, threat.x, threat.y, threat.z);
}
}

void EmpiresNoteFriendlyDeath(edict_t* victim, edict_t* attacker)
{
	if (!rcbot_empires_self_preservation.GetBool() || engine == nullptr || gpGlobals == nullptr ||
		victim == nullptr || attacker == nullptr || victim == attacker)
		return;
	const int killer = engine->IndexOfEdict(attacker);
	if (killer <= 0 || killer > gpGlobals->maxClients)
		return; // world, turret, prop: no soldier to fear at a spot (turrets have their own handling)
	const int team = CClassInterface::getTeam(victim);
	const int killerTeam = CClassInterface::getTeam(attacker);
	if (team < 2 || team > 3 || killerTeam < 2 || killerTeam > 3 || killerTeam == team)
		return;
	NoteNearDeath(team, CBotGlobals::entityOrigin(victim), CBotGlobals::entityOrigin(attacker),
		engine->GetPlayerUserId(victim), engine->GetPlayerUserId(attacker));
}

void EmpiresInjectNearDeath(int team, const Vector& at, const Vector& threat)
{
	NoteNearDeath(team, at, threat, -1, -1);
}

bool CBotEmpires::empiresShaken(float now)
{
	if (!rcbot_empires_self_preservation.GetBool())
		return false;
	if (now < m_flShakenUntil)
		return true;
	const int team = getTeam();
	if (now < m_flShakenNext || team < 2 || team > 3 || engine == nullptr)
		return false;
	const Vector origin = getOrigin();
	const float me[3] = { origin.x, origin.y, origin.z };
	float newest = 0.0f, threat[3];
	const int deaths = EmpiresTeamDirector::CountNearDeaths(s_nearDeaths[team], me, now, m_flShakenAfter, newest, threat);
	if (deaths < EmpiresTeamDirector::kShakenDeaths)
		return false;
	m_flShakenAfter = newest;
	m_flShakenStarted = now;
	m_flShakenUntil = now + kShakenSeconds;
	m_flShakenNext = now + kShakenCooldown;
	m_vShakenThreat = Vector(threat[0], threat[1], threat[2]);
	m_vShakenStart = origin;
	m_bShakenHasGoal = false;
	m_iShakenStage = 0;
	RCBOT_DIAG("[RCBOT2] shaken start userid=%d deaths=%d at=(%.0f %.0f %.0f) threat=(%.0f %.0f %.0f) threat_dist=%.0f fight=%d\n",
		engine->GetPlayerUserId(m_pEdict), deaths, origin.x, origin.y, origin.z, threat[0], threat[1], threat[2],
		(m_vShakenThreat - origin).Length2D(), m_bSkirmishActive ? 1 : 0);
	return true;
}

bool CBotEmpires::pickEmpiresShakenSpot(const Vector& origin, bool avoidCurrent, Vector& out) const
{
	Vector away = origin - m_vShakenThreat;
	away.z = 0.0f;
	if (away.Length2D() < 1.0f)
		away = Vector(1.0f, 0.0f, 0.0f);
	away.NormalizeInPlace();
	const Vector side(-away.y, away.x, 0.0f);
	const float s = randomInt(0, 1) == 0 ? -1.0f : 1.0f;
	const Vector dirs[] = { away, (away + side * s) * 0.7071f, (away - side * s) * 0.7071f, side * s, side * -s };
	const Vector eye = m_vShakenThreat + Vector(0, 0, 60);
	// First a spot the killer cannot see, then any safe spot farther from him.
	for (int pass = 0; pass < 2; ++pass)
	{
		for (const float range : {300.0f, 450.0f, 200.0f})
		{
			for (const Vector& dir : dirs)
			{
				Vector spot;
				if (!empiresSkirmishSpotSafe(origin, origin + dir * range, nullptr, spot))
					continue;
				if (avoidCurrent && (spot - m_vShakenGoal).Length2D() < 120.0f)
					continue;
				if (pass == 0 && CBotGlobals::isVisible(m_pEdict, eye, spot + Vector(0, 0, 48)))
					continue;
				out = spot;
				return true;
			}
		}
	}
	return false;
}

void CBotEmpires::endEmpiresShaken(const char* reason)
{
	if (!m_bShakenActive)
		return;
	m_bShakenActive = false;
	const float now = gpGlobals != nullptr ? gpGlobals->curtime : 0.0f;
	RCBOT_DIAG("[RCBOT2] shaken end userid=%d reason=%s secs=%.1f moved=%.0f threat_dist=%.0f stage=%d\n",
		engine != nullptr ? engine->GetPlayerUserId(m_pEdict) : -1, reason, now - m_flShakenStarted,
		(getOrigin() - m_vShakenStart).Length2D(), (m_vShakenThreat - getOrigin()).Length2D(), m_iShakenStage);
}

bool CBotEmpires::handleEmpiresShaken()
{
	if (gpGlobals == nullptr || engine == nullptr || m_pSchedules == nullptr)
		return false;
	const float now = gpGlobals->curtime;
	if (!empiresShaken(now))
	{
		endEmpiresShaken("time");
		return false;
	}
	// A fight in the weapon band handles it (skirmish backs off); busy actions finish first.
	if (m_bSkirmishActive || EmpiresIsEngibot(m_pEdict) || m_iAmmoCrateStage != 0 ||
		m_iEmpiresBuildingActionMode != 0 || lawlietBlocksWeapons() || EmpiresVehiclePlayerVehicle(this, m_pEdict) >= 0)
	{
		endEmpiresShaken(m_bSkirmishActive ? "fight" : "busy");
		return false;
	}
	const Vector origin = getOrigin();
	const int userId = engine->GetPlayerUserId(m_pEdict);
	if (!m_bShakenActive)
	{
		m_bShakenActive = true;
		takeEmpiresMovement("shaken");
		Vector spot;
		if (pickEmpiresShakenSpot(origin, false, spot))
		{
			m_vShakenGoal = spot;
			m_bShakenHasGoal = true;
			m_flShakenGoalSet = now;
			RCBOT_DIAG("[RCBOT2] shaken fall-back userid=%d at=(%.0f %.0f %.0f) spot=(%.0f %.0f %.0f) hidden=%d\n",
				userId, origin.x, origin.y, origin.z, spot.x, spot.y, spot.z,
				CBotGlobals::isVisible(m_pEdict, m_vShakenThreat + Vector(0, 0, 60), spot + Vector(0, 0, 48)) ? 0 : 1);
		}
		else
		{
			m_iShakenStage = 2;
			RCBOT_DIAG("[RCBOT2] shaken no-spot userid=%d at=(%.0f %.0f %.0f) reject=%d\n", userId, origin.x, origin.y,
				origin.z, m_iSkirmishRejectMask);
		}
	}
	// Local progress check: blocked on the way -> another spot -> crouch where we are.
	if (m_bShakenHasGoal && m_iShakenStage < 2 && now - m_flShakenGoalSet > kShakenProgress &&
		m_vVelocity.Length2D() < kMinStepSpeed && (m_vShakenGoal - origin).Length2D() > 32.0f)
	{
		Vector spot;
		if (m_iShakenStage == 0 && pickEmpiresShakenSpot(origin, true, spot))
		{
			m_iShakenStage = 1;
			m_vShakenGoal = spot;
			m_flShakenGoalSet = now;
			RCBOT_DIAG("[RCBOT2] stall-escalate who=shaken stage=1 reason=blocked userid=%d at=(%.0f %.0f %.0f) spot=(%.0f %.0f %.0f)\n",
				userId, origin.x, origin.y, origin.z, spot.x, spot.y, spot.z);
		}
		else
		{
			m_iShakenStage = 2;
			RCBOT_DIAG("[RCBOT2] stall-escalate who=shaken stage=2 reason=blocked userid=%d at=(%.0f %.0f %.0f)\n",
				userId, origin.x, origin.y, origin.z);
		}
	}

	const int saved = m_iMoveLookPriority;
	setMoveLookPriority(MOVELOOK_OVERRIDE);
	if (m_bShakenHasGoal && m_iShakenStage < 2 && (m_vShakenGoal - origin).Length2D() > 24.0f)
		setMoveTo(m_vShakenGoal);
	else
	{
		stopMoving();
		if (m_pButtons != nullptr)
			m_pButtons->duck(0.1f);
	}
	edict_t* enemy = m_pEnemy.get();
	if (enemy != nullptr && hasSomeConditions(CONDITION_SEE_CUR_ENEMY) && CBotGlobals::entityIsAlive(enemy))
		setLookAtTask(LOOK_ENEMY);
	else
	{
		setLookVector(m_vShakenThreat + Vector(0, 0, 48));
		setLookAtTask(LOOK_VECTOR);
	}
	setMoveLookPriority(saved);
	return true;
}
