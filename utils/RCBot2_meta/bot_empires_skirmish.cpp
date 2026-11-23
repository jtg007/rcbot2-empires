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
#include "bot_globals.h"
#include "bot_getprop.h"
#include "bot_navmesh_api.h"
#include "bot_schedule.h"
#include "bot_buttons.h"
#include "bot_cvars.h"
#include "bot_weapons.h"
#include "iplayerinfo.h"
#include <algorithm>
#include <cmath>
#include <cstring>

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
constexpr float kCrouchRange = 500.0f;     // short crouched bursts only beyond this
constexpr float kTooClose = 110.0f;        // always back off inside this
constexpr float kSightGrace = 1.0f;        // keep moving like in the fight this long after losing sight

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

// Preferred distance band from the weapon's range; a shotgun scout closes in, a rifleman stays back.
void SkirmishBand(const CBotWeapon* weapon, float& nearDist, float& farDist)
{
	float maxRange = weapon != nullptr ? weapon->getPrimaryMaxRange() : 0.0f;
	if (!(maxRange > 0.0f))
		maxRange = 1200.0f;
	// Effective, not maximum range: beyond ~900 the approach keeps closing in.
	farDist = std::clamp(maxRange * 0.5f, 300.0f, 900.0f);
	nearDist = std::clamp(farDist * 0.3f, 120.0f, 300.0f);
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

void CBotEmpires::endEmpiresSkirmish(const char* reason)
{
	if (!m_bSkirmishActive)
		return;
	const float now = gpGlobals != nullptr ? gpGlobals->curtime : 0.0f;
	RCBOT_DIAG("[RCBOT2] skirmish end userid=%d reason=%s secs=%.1f steps=%d blocked=%d pauses=%d covers=%d odds_back=%d\n",
		engine != nullptr ? engine->GetPlayerUserId(m_pEdict) : -1, reason, now - m_flSkirmishStarted,
		m_iSkirmishSteps, m_iSkirmishBlocks, m_iSkirmishPauses, m_iSkirmishCovers, m_iSkirmishOddsBack);
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
	const bool soldier = valid && enemyIndex > 0 && enemyIndex <= gpGlobals->maxClients &&
		EmpiresVehiclePlayerVehicle(this, enemy) < 0;
	if (!soldier)
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

	float nearDist, farDist;
	SkirmishBand(getCurrentWeapon(), nearDist, farDist);
	// Grenadiers mortar from 900 on; leave that range to the mortar state machine.
	int classId = -1;
	bool desired = false, desiredKnown = false;
	const char* prop = nullptr;
	if (readEmpiresClass(m_pEdict, classId, desired, desiredKnown, prop) && classId == 2)
		farDist = (std::min)(farDist, 880.0f);
	// Hysteresis: once skirmishing, the approach takes over only well outside the band.
	const float leaveDist = m_bSkirmishActive ? farDist * 1.25f : farDist;
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
		m_bSkirmishNoStepLogged = false;
		m_flSkirmishOddsAt = 0.0f;
		m_iSkirmishStage = 0;
		m_iSkirmishSide = randomInt(0, 1) == 0 ? -1 : 1;
		RCBOT_DIAG("[RCBOT2] skirmish start userid=%d enemy_userid=%d dist=%.0f band=%.0f-%.0f\n",
			engine->GetPlayerUserId(m_pEdict), engine->GetPlayerUserId(enemy), dist, nearDist, farDist);
		calloutEmpiresEnemy(enemy);
	}
	if (seen)
		postEmpiresContact(CBotGlobals::entityOrigin(enemy));
	// Take the movement over from the approach route and any navigation intent, once.
	if (m_bEmpiresApproachTaskIssued || !m_pSchedules->isEmpty() || m_navigationIntent.active)
	{
		m_bEmpiresApproachTaskIssued = false;
		m_pEmpiresApproachTarget = nullptr;
		cancelNavigationAttempt("skirmish");
		// A cancelled provider can hand the intent to a fallback provider that keeps owning the
		// movement and silently rejects setMoveTo; end the intent outright.
		if (m_navigationIntent.active)
			finishNavigationIntent(RCBotNavigation::Result::CANCELLED, "skirmish");
		m_pSchedules->freeMemory();
		if (m_pNavigator != nullptr)
			m_pNavigator->clear();
	}

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
			!(m_iSkirmishEnemies >= m_iSkirmishAllies + 2))
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
		const bool outnumbered = useOdds && enemies >= allies + 2;
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
		const Vector enemyPos = CBotGlobals::entityOrigin(enemy);
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

		bool pause = m_iSkirmishStage >= 1;
		if (!pause && mode[0] == 's' && dist > kCrouchRange && m_iSkirmishPhase == 1 &&
			randomFloat(0.0f, 1.0f) < 0.2f)
			pause = true;

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
				userId, reloading ? "reload" : hurtBadly ? "hurt" : "outnumbered", origin.x, origin.y, origin.z,
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
	}
	m_bSkirmishActive = false;
	m_pSkirmishEnemy = nullptr;
	m_iSkirmishPhase = 0;
	m_flSkirmishDisabledUntil = 0.0f;
	m_flSkirmishTick = 0.0f;
	m_flSkirmishOddsAt = 0.0f;
	m_flNextCallout = 0.0f;
	m_flNextContactPost = 0.0f;
	m_iJoinContactId = 0;
	m_iJoinSkipId = 0;
	m_flJoinNextNav = 0.0f;
	m_flJoinNextSearch = 0.0f;
}
