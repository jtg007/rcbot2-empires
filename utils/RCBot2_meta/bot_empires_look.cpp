// Infantry realistic look and combat polish (4 independently gated stages):
//   Stage 1 (rcbot_empires_smooth_look): decoupled critically-damped pitch/yaw
//     spring-damper + content-based look priorities (fixes shared m_fAimMoment
//     cross-axis wobble and phase-only look locking).
//   Stage 2 (rcbot_empires_nav_lookahead): level navmesh lookahead on slopes +
//     corner-bent chest-height LOS hold + bounded last-enemy extrapolation.
//   Stage 3 (rcbot_empires_smart_idle_look): anti-wall horizontal ray scan when
//     standing/holding + eye-level noise look + subtle per-bot idle drift.
//   Stage 4 (rcbot_empires_combat_polish): proactive tactical reload between
//     fights + asymmetric fire-alignment gate + long-range automatic bursts.
#include "bot_empires.h"
#include "bot_empires_look_logic.h"
#include "bot_buttons.h"
#include "bot_cvars.h"
#include "bot_getprop.h"
#include "bot_globals.h"
#include "bot_navmesh_provider.h"
#include "bot_profile.h"
#include "bot_weapons.h"
#include "iplayerinfo.h"

#include <algorithm>
#include <cmath>
#include <cstddef>

int EmpiresVehiclePlayerVehicle(const CBotEmpires* bot, const edict_t* player); // bot_empires_vehicle.cpp

extern IVEngineServer* engine;
extern IEngineTrace* enginetrace;
extern CGlobalVars* gpGlobals;
extern IPlayerInfoManager* playerinfomanager;

namespace
{

EmpiresLookLogic::LookPriority PriorityForTask(const eLookTask task)
{
	switch (task)
	{
	case LOOK_ENEMY:
		return EmpiresLookLogic::LookPriority::CRITICAL;
	case LOOK_HURT_ORIGIN:
		return EmpiresLookLogic::LookPriority::URGENT;
	case LOOK_VECTOR:
	case LOOK_EDICT:
	case LOOK_BUILD:
	case LOOK_SNIPE:
		return EmpiresLookLogic::LookPriority::IMPORTANT;
	case LOOK_LAST_ENEMY:
	case LOOK_NOISE:
	case LOOK_WAYPOINT_AIM:
		return EmpiresLookLogic::LookPriority::INTERESTING;
	case LOOK_NONE:
	case LOOK_AROUND:
	case LOOK_WAYPOINT:
	case LOOK_WAYPOINT_NEXT_ONLY:
	case LOOK_GROUND:
	default:
		return EmpiresLookLogic::LookPriority::BORING;
	}
}

// ReGameDLL BendLineOfSight (cs_bot_vision.cpp): when the last-seen enemy spot
// is occluded behind a wall, sweep horizontal rays in 15-deg steps (+/-15..120)
// to find the nearest corner point with LOS to the target so the bot holds the
// doorway/corner instead of staring into the wall.
bool BendLineOfSightToCorner(edict_t* selfEdict, const Vector& eye, const Vector& target, Vector& outPoint)
{
	if (enginetrace == nullptr || selfEdict == nullptr)
		return false;

	CTraceFilterWorldAndPropsOnly filter;
	trace_t tr;
	Ray_t ray;
	ray.Init(eye, target);
	enginetrace->TraceRay(ray, MASK_SOLID_BRUSHONLY, &filter, &tr);
	if (!tr.startsolid && tr.fraction >= 0.98f)
	{
		outPoint = target;
		return true;
	}

	Vector to = target - eye;
	to.z = 0.0f;
	const float dist2D = to.Length2D();
	if (dist2D < 32.0f || dist2D > 2000.0f)
		return false;
	const float baseYaw = std::atan2(to.y, to.x);
	const float probeLen = (std::min)(dist2D, 450.0f);

	for (float offsetDeg = 15.0f; offsetDeg <= 120.0f; offsetDeg += 15.0f)
	{
		for (int side = 0; side < 2; ++side)
		{
			const float yaw = baseYaw + (side == 0 ? offsetDeg : -offsetDeg) * EmpiresLookLogic::kDegToRad;
			const Vector dir(std::cos(yaw), std::sin(yaw), 0.0f);
			const Vector end = eye + dir * probeLen;

			ray.Init(eye, end);
			enginetrace->TraceRay(ray, MASK_SOLID_BRUSHONLY, &filter, &tr);
			if (tr.startsolid)
				continue;
			const float visLen = probeLen * tr.fraction;
			for (float step = 64.0f; step <= visLen - 16.0f; step += 64.0f)
			{
				const Vector corner = eye + dir * step;
				ray.Init(corner, target);
				trace_t check;
				enginetrace->TraceRay(ray, MASK_SOLID_BRUSHONLY, &filter, &check);
				if (!check.startsolid && check.fraction >= 0.98f)
				{
					outPoint = corner;
					return true;
				}
			}
		}
	}
	return false;
}

} // namespace

void CBotEmpires::resetEmpiresLookState()
{
	m_lookPitchSpring.Reset();
	m_lookYawSpring.Reset();
	m_lookHeadSteady.Reset();
	m_flLookSpringLastTime = 0.0f;
	m_eLookContentPriority = EmpiresLookLogic::LookPriority::BORING;
	m_flLookContentExpireAt = 0.0f;
	m_flVerticalLookHoldUntil = 0.0f;
	m_flNavLookLogAt = 0.0f;
	m_flSmartIdleLogAt = 0.0f;
	m_iSmartIdleWallPicks = 0;
	m_iSmartIdleOpenPicks = 0;
	m_flCombatOffTargetSince = -1.0f;
	m_flCombatBurstStart = -1.0f;
	m_flCombatPolishLogAt = 0.0f;
	m_flTacticalReloadNextCheck = 0.0f;
	for (int& maxClip : m_iMaxClipSeen)
		maxClip = 0;
}

bool CBotEmpires::empiresInfantryLookContext() const
{
	if (m_pEdict == nullptr || m_pEdict->IsFree() || m_pPlayerInfo == nullptr || m_pPlayerInfo->IsDead())
		return false;
	if (empiresVehicleOwnsBot() || EmpiresVehiclePlayerVehicle(this, m_pEdict) >= 0)
		return false;
	return true;
}

bool CBotEmpires::handleCustomLookAtTask(const eLookTask lookTask, const float fTime)
{
	if (!rcbot_empires_smooth_look.GetBool() || !empiresInfantryLookContext() || engine == nullptr)
		return false;

	const float now = engine->Time();
	const EmpiresLookLogic::LookPriority newPri = PriorityForTask(lookTask);
	const bool sameTask = (m_iLookTask == lookTask);
	const bool accept = EmpiresLookLogic::ShouldAcceptLookRequest(
		m_eLookContentPriority,
		m_flLookContentExpireAt,
		newPri,
		sameTask,
		now,
		m_lookHeadSteady.IsSteady(),
		m_lookHeadSteady.SteadyDuration(now));
	if (!accept)
		return true; // handled (rejected by content priority)

	m_iLookTask = lookTask;
	m_iLookPriority = m_iMoveLookPriority;
	m_eLookContentPriority = newPri;
	if (fTime > 0.0f)
	{
		m_fLookSetTime = now + fTime;
		m_flLookContentExpireAt = now + fTime;
	}
	else if (!sameTask)
	{
		// Keep a short hold window for urgent/important tasks unless explicitly timed.
		const float hold = newPri >= EmpiresLookLogic::LookPriority::IMPORTANT ? 0.15f : 0.0f;
		m_fLookSetTime = now + hold;
		m_flLookContentExpireAt = now + hold;
	}
	return true;
}

bool CBotEmpires::applyCustomLookAngles(const QAngle& requiredAngles, QAngle& viewAngles)
{
	if (!rcbot_empires_smooth_look.GetBool() || !empiresInfantryLookContext() || engine == nullptr)
		return false;

	const float now = engine->Time();
	float dt = gpGlobals != nullptr && gpGlobals->frametime > 0.0005f ? gpGlobals->frametime : 0.015f;
	if (m_flLookSpringLastTime > 0.0f && now > m_flLookSpringLastTime)
		dt = (std::min)(now - m_flLookSpringLastTime, 0.1f);
	m_flLookSpringLastTime = now;

	const bool combat = (m_iLookTask == LOOK_ENEMY) ||
		(m_pEnemy.get() != nullptr && hasSomeConditions(CONDITION_SEE_CUR_ENEMY));
	const bool urgent = (m_iLookTask == LOOK_HURT_ORIGIN) || onLadder() || m_bIncreaseSensitivity;
	const float skill01 = m_pProfile != nullptr ? m_pProfile->m_fAimSkill : 0.8f;

	float targetPitch = requiredAngles.x;
	float targetYaw = requiredAngles.y;

	// Subtle per-bot idle drift when standing calmly (YaPB cv_aim_drift).
	if (rcbot_empires_smart_idle_look.GetBool() && !combat && !urgent &&
		m_iLookTask == LOOK_AROUND && !moveToIsValid())
	{
		const CBotWeapon* weapon = getCurrentWeapon();
		const bool sniper = weapon != nullptr && weapon->getWeaponInfo() != nullptr &&
			weapon->getWeaponInfo()->isScoped();
		float dYaw = 0.0f, dPitch = 0.0f;
		EmpiresLookLogic::ComputeIdleDrift(now, engine->IndexOfEdict(m_pEdict), sniper, dYaw, dPitch);
		targetYaw = EmpiresLookLogic::NormalizeAngle(targetYaw + dYaw);
		targetPitch = EmpiresLookLogic::ClampFloat(targetPitch + dPitch, -89.0f, 89.0f);
	}

	// Freeze pitch when non-combat look target is nearly straight above/below (< 48 HU 2D).
	const bool freezePitch = !combat && !urgent && !onLadder() &&
		m_iLookTask != LOOK_GROUND && (m_vLookAt - getEyePosition()).Length2D() < 48.0f;

	float pitch = viewAngles.x;
	float yaw = viewAngles.y;
	EmpiresLookLogic::StepViewAngles(m_lookPitchSpring, m_lookYawSpring,
		pitch, yaw, targetPitch, targetYaw, dt, combat, urgent, skill01, freezePitch);

	viewAngles.x = pitch;
	viewAngles.y = yaw;
	m_lookHeadSteady.Update(now, dt, pitch, yaw);
	return true;
}

bool CBotEmpires::resolveEmpiresRealisticLookVector()
{
	if (!empiresInfantryLookContext() || engine == nullptr)
		return false;

	const float now = engine->Time();
	const Vector eye = getEyePosition();
	const Vector origin = getOrigin();

	// Stage 3: Lift LOOK_NOISE to chest height instead of feet.
	if (m_iLookTask == LOOK_NOISE && rcbot_empires_smart_idle_look.GetBool())
	{
		if (m_pEnemy.get() != nullptr && hasSomeConditions(CONDITION_SEE_CUR_ENEMY))
		{
			setLookAtTask(LOOK_ENEMY);
			return false;
		}
		if (!m_bListenPositionValid || m_fListenTime < now)
		{
			setLookAtTask(LOOK_WAYPOINT);
			return false;
		}
		setLookAt(m_vListenPosition + Vector(0.0f, 0.0f, EmpiresLookLogic::kLastEnemyChestLift));
		return true;
	}

	// Stage 2: Level navmesh lookahead + corner-bent chest-height LOS hold.
	if (rcbot_empires_nav_lookahead.GetBool())
	{
		const bool navMoving = RCBotNavMeshProvider::IsActive(this) && moveToIsValid();
		// A stale LOOK_LAST_ENEMY or LOOK_WAYPOINT (or LOOK_AROUND while actively walking a navmesh route).
		if (m_iLookTask == LOOK_LAST_ENEMY)
		{
			if (m_pLastEnemy.get() != nullptr)
			{
				Vector target = m_vLastSeeEnemy + Vector(0.0f, 0.0f, EmpiresLookLogic::kLastEnemyChestLift);
				Vector corner;
				if (BendLineOfSightToCorner(m_pEdict, eye, target, corner))
					target = corner;
				setLookAt(target);
				return true;
			}
		}
		else if (m_iLookTask == LOOK_WAYPOINT || m_iLookTask == LOOK_WAYPOINT_NEXT_ONLY ||
			(m_iLookTask == LOOK_AROUND && navMoving))
		{
			// Own recent LOS loss: hold the last seen spot (or corner toward it) at chest height for 2.5 s.
			const bool selfSighted = m_pLastEnemy.get() != nullptr && !m_bEmpiresLastEnemyReported;
			const float lastDist2D = selfSighted ? (m_vLastSeeEnemy - origin).Length2D() : 0.0f;
			if (EmpiresLookLogic::ShouldHoldLastEnemyLook(selfSighted, now, m_fLastSeeEnemy,
				EmpiresLookLogic::kLastEnemyHoldSec, lastDist2D))
			{
				Vector target = m_vLastSeeEnemy + Vector(0.0f, 0.0f, EmpiresLookLogic::kLastEnemyChestLift);
				Vector corner;
				if (BendLineOfSightToCorner(m_pEdict, eye, target, corner))
					target = corner;
				setLookAt(target);
				return true;
			}

			// Active navmesh route: smooth polyline lookahead kept level on flat/gentle slopes.
			Vector lookTarget;
			bool vertical = onLadder();
			if (vertical || m_vVelocity.z > 48.0f)
				m_flVerticalLookHoldUntil = now + 0.2f;
			const bool verticalHeld = vertical || now < m_flVerticalLookHoldUntil;

			if (RCBotNavMeshProvider::GetLookaheadTarget(this, EmpiresLookLogic::kDefaultNavLookaheadDist,
				lookTarget, vertical))
			{
				if (!vertical && !verticalHeld)
				{
					// Already level-clamped by PickNavLookaheadPoint.
				}
				setLookAt(lookTarget);
				if (gpGlobals != nullptr && gpGlobals->curtime >= m_flNavLookLogAt)
				{
					m_flNavLookLogAt = gpGlobals->curtime + 10.0f;
					QAngle want;
					VectorAngles(lookTarget - eye, want);
					CBotGlobals::fixFloatAngle(&want.x);
					RCBOT_DIAG("[RCBOT2] nav-look userid=%d pitch=%.1f yaw=%.1f vert=%d\n",
						engine->GetPlayerUserId(m_pEdict), want.x, want.y, vertical ? 1 : 0);
				}
				return true;
			}

			// Fallback when moving without waypoints or navmesh provider (e.g. direct setMoveTo):
			// look toward m_vMoveTo at eye level instead of staring into the ground.
			if (moveToIsValid() && (m_vMoveTo - origin).Length2D() > 32.0f &&
				(m_pNavigator == nullptr || !m_pNavigator->hasNextPoint()))
			{
				Vector target = m_vMoveTo;
				if (!verticalHeld)
					target.z = eye.z;
				else
					target.z += EmpiresLookLogic::kLastEnemyChestLift;
				setLookAt(target);
				return true;
			}
		}
	}

	// Stage 3: Smart anti-wall idle look when standing/holding.
	if (m_iLookTask == LOOK_AROUND && rcbot_empires_smart_idle_look.GetBool() && enginetrace != nullptr)
	{
		// Check whether we are currently staring straight into a close wall.
		CTraceFilterWorldAndPropsOnly filter;
		Vector curFwd;
		AngleVectors(QAngle(0.0f, m_vViewAngles.y, 0.0f), &curFwd);
		Ray_t curRay;
		trace_t curTr;
		curRay.Init(eye, eye + curFwd * EmpiresLookLogic::kIdleMaxRayDist);
		enginetrace->TraceRay(curRay, MASK_SOLID_BRUSHONLY, &filter, &curTr);
		const float currentClear = curTr.startsolid ? 0.0f : curTr.fraction * EmpiresLookLogic::kIdleMaxRayDist;
		const bool facingWallNow = currentClear < EmpiresLookLogic::kIdleMinWallClearance;

		if (m_fLookAroundTime < now || facingWallNow)
		{
			// If we recently saw an enemy ourselves (< 20 s), check if we can hold them or their corner.
			const bool hasRecentOwnThreat = m_pLastEnemy.get() != nullptr && !m_bEmpiresLastEnemyReported &&
				(now - m_fLastSeeEnemy) < 20.0f && (m_vLastSeeEnemy - origin).Length2D() > 64.0f;
			float threatYaw = 0.0f;
			if (hasRecentOwnThreat)
			{
				const Vector toThreat = m_vLastSeeEnemy - origin;
				threatYaw = std::atan2(toThreat.y, toThreat.x) * EmpiresLookLogic::kRadToDeg;
			}

			constexpr int kNumRays = 8;
			EmpiresLookLogic::IdleRaySample samples[kNumRays];
			for (int i = 0; i < kNumRays; ++i)
			{
				const float yawDeg = EmpiresLookLogic::NormalizeAngle(m_vViewAngles.y + static_cast<float>(i) * 45.0f);
				Vector dir;
				AngleVectors(QAngle(0.0f, yawDeg, 0.0f), &dir);
				Ray_t ray;
				trace_t tr;
				ray.Init(eye, eye + dir * EmpiresLookLogic::kIdleMaxRayDist);
				enginetrace->TraceRay(ray, MASK_SOLID_BRUSHONLY, &filter, &tr);
				samples[i].yawDeg = yawDeg;
				samples[i].clearDist = tr.startsolid ? 0.0f : tr.fraction * EmpiresLookLogic::kIdleMaxRayDist;
				if (hasRecentOwnThreat)
				{
					const float diff = std::fabs(EmpiresLookLogic::AngleDiff(yawDeg, threatYaw));
					samples[i].threatBias = EmpiresLookLogic::ClampFloat(1.0f - diff / 90.0f, 0.0f, 1.0f);
				}
				else
				{
					samples[i].threatBias = 0.0f;
				}
			}

			float chosenYaw = m_vViewAngles.y;
			const float jitter = randomFloat(0.0f, 1.0f);
			EmpiresLookLogic::PickSmartIdleYaw(samples, kNumRays, m_vViewAngles.y, currentClear,
				EmpiresLookLogic::kIdleMinWallClearance, jitter, chosenYaw);

			Vector chosenFwd;
			AngleVectors(QAngle(0.0f, chosenYaw, 0.0f), &chosenFwd);
			m_vLookAroundOffset = eye + chosenFwd * 512.0f;
			m_fLookAroundTime = now + randomFloat(3.0f, 5.0f);

			if (facingWallNow)
				++m_iSmartIdleWallPicks;
			else
				++m_iSmartIdleOpenPicks;

			if (gpGlobals != nullptr && gpGlobals->curtime >= m_flSmartIdleLogAt)
			{
				m_flSmartIdleLogAt = gpGlobals->curtime + 30.0f;
				RCBOT_DIAG("[RCBOT2] smart-idle-look userid=%d wall_escapes=%d open_picks=%d cur_clear=%.0f chosen_yaw=%.0f\n",
					engine->GetPlayerUserId(m_pEdict), m_iSmartIdleWallPicks, m_iSmartIdleOpenPicks,
					currentClear, chosenYaw);
				m_iSmartIdleWallPicks = 0;
				m_iSmartIdleOpenPicks = 0;
			}
		}
		setLookAt(m_vLookAroundOffset);
		return true;
	}

	return false;
}

void CBotEmpires::tickEmpiresTacticalReload()
{
	if (!rcbot_empires_combat_polish.GetBool() || !empiresInfantryLookContext() ||
		engine == nullptr || m_pWeapons == nullptr)
		return;

	CBotWeapon* weapon = getCurrentWeapon();
	if (weapon == nullptr || weapon->getWeaponInfo() == nullptr)
		return;

	const int wid = weapon->getID();
	const int clip1 = weapon->getClip1(this);
	if (wid >= 0 && wid < kEmpiresMaxTrackedWeapons && clip1 > m_iMaxClipSeen[wid])
		m_iMaxClipSeen[wid] = clip1;

	const float now = engine->Time();
	if (now < m_flTacticalReloadNextCheck)
		return;
	m_flTacticalReloadNextCheck = now + 0.5f;

	const bool hasVisibleEnemy = m_pEnemy.get() != nullptr && hasSomeConditions(CONDITION_SEE_CUR_ENEMY);
	const float sinceEnemy = m_fLastSeeEnemy > 0.0f ? (now - m_fLastSeeEnemy) : 100.0f;
	const bool busyAction = m_iAmmoCrateStage != 0 || m_deploy.stage != 0 ||
		m_iEmpiresBuildingActionMode != 0 || m_iReviveTarget != 0 || m_iHealTarget != 0 ||
		m_bHealAttacking || m_bWreckClearAttacking || isEmpiresMortarActionActive() ||
		isEmpiresScoutBinocularActionActive() || isEmpiresEngineerBuildActionActive() ||
		isEmpiresEngineerRepairActionActive() || isEmpiresMineActionActive() ||
		m_iCommanderEntryStage != 0 || m_iCommanderBuildStage != 0;
	const bool meleeOrExplosive = weapon->isMelee() || weapon->isExplosive() || weapon->isSpecial();
	const int maxClip = (wid >= 0 && wid < kEmpiresMaxTrackedWeapons) ? m_iMaxClipSeen[wid] : 0;
	const int reserve = weapon->getAmmo(this);

	if (EmpiresLookLogic::ShouldTacticalReload(hasVisibleEnemy, sinceEnemy, clip1, maxClip,
		reserve, busyAction, meleeOrExplosive))
	{
		reload();
		m_flTacticalReloadNextCheck = now + 3.0f;
		RCBOT_DIAG("[RCBOT2] tactical-reload userid=%d weapon=%s clip=%d/%d reserve=%d quiet=%.1f\n",
			engine->GetPlayerUserId(m_pEdict), weapon->getWeaponInfo()->getWeaponName(),
			clip1, maxClip, reserve, sinceEnemy);
	}
}

bool CBotEmpires::empiresCombatFireReady(edict_t* pEnemy, CBotWeapon* pWeapon)
{
	if (!rcbot_empires_combat_polish.GetBool() || pEnemy == nullptr || pWeapon == nullptr ||
		pWeapon->getWeaponInfo() == nullptr || engine == nullptr)
		return true;

	const float now = engine->Time();
	const Vector eye = getEyePosition();
	const Vector targetPos = CBotGlobals::entityOrigin(pEnemy) + Vector(0.0f, 0.0f, 36.0f);
	Vector toEnemy = targetPos - eye;
	const float dist2D = toEnemy.Length2D();
	const float dist3D = toEnemy.Length();
	if (dist3D < 1.0f)
		return true;
	toEnemy /= dist3D;

	Vector myFwd;
	AngleVectors(CBotGlobals::playerAngles(m_pEdict), &myFwd); // CBot::eyeAngles() is inline in bot.cpp
	const float viewDot = DotProduct(myFwd, toEnemy);

	// Check whether the enemy is facing us (players only; buildings/vehicles count as facing us
	// so we never delay anti-turret/vehicle fire).
	float enemyDot = 1.0f;
	const int enemyIdx = engine->IndexOfEdict(pEnemy);
	if (enemyIdx > 0 && enemyIdx <= gpGlobals->maxClients && playerinfomanager != nullptr)
	{
		IPlayerInfo* info = playerinfomanager->GetPlayerInfo(pEnemy);
		if (info != nullptr)
		{
			Vector enemyFwd;
			AngleVectors(info->GetAbsAngles(), &enemyFwd);
			Vector enemyToMe = getOrigin() - CBotGlobals::entityOrigin(pEnemy);
			enemyToMe.z = 0.0f;
			const float len = enemyToMe.Length2D();
			if (len > 1.0f)
			{
				enemyToMe /= len;
				enemyFwd.z = 0.0f;
				const float fl = enemyFwd.Length2D();
				if (fl > 0.01f)
					enemyDot = (enemyFwd.x * enemyToMe.x + enemyFwd.y * enemyToMe.y) / fl;
			}
		}
	}

	const bool melee = pWeapon->isMelee();
	const bool scoped = pWeapon->getWeaponInfo()->isScoped();
	const float offElapsed = m_flCombatOffTargetSince >= 0.0f ? (now - m_flCombatOffTargetSince) : 0.0f;

	if (!EmpiresLookLogic::ShouldAllowCombatFire(dist2D, viewDot, enemyDot, offElapsed, melee, scoped))
	{
		if (m_flCombatOffTargetSince < 0.0f)
			m_flCombatOffTargetSince = now;
		m_flCombatBurstStart = -1.0f;
		if (gpGlobals != nullptr && gpGlobals->curtime >= m_flCombatPolishLogAt)
		{
			m_flCombatPolishLogAt = gpGlobals->curtime + 5.0f;
			RCBOT_DIAG("[RCBOT2] fire-hold userid=%d reason=align dist=%.0f dot=%.3f enemy_dot=%.2f\n",
				engine->GetPlayerUserId(m_pEdict), dist2D, viewDot, enemyDot);
		}
		return false;
	}

	m_flCombatOffTargetSince = -1.0f;

	// Long-range automatic burst cadence (> 750 HU): short recoil pause to improve hit rate.
	const bool automatic = pWeapon->mustHoldAttack() && !melee && !pWeapon->isExplosive();
	if (m_flCombatBurstStart < 0.0f)
		m_flCombatBurstStart = now;
	if (EmpiresLookLogic::ShouldPauseForLongRangeBurst(dist2D, automatic, scoped, now - m_flCombatBurstStart))
		return false;

	return true;
}
