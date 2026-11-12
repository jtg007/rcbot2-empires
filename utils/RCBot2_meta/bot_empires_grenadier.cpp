#include "bot_empires.h"
#include "bot_empires_telemetry.h"
#include "bot_weapons.h"
#include "in_buttons.h"
#include "bot_buttons.h"

#include "bot_getprop.h"
#include "bot_globals.h"
#include "bot_navigator.h"
#include "bot_profile.h"
#include "bot_schedule.h"
#include "bot_waypoint.h"
#include "bot_waypoint_locations.h"
#include "rcbot/logging.h"

#include "dt_send.h"
#include "server_class.h"
#include "const.h"
#include "engine/IEngineTrace.h"

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wreorder"
#endif
#include "ientityinfo.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#include <algorithm>
#include <cmath>
#include <cstring>

namespace
{
	constexpr float kEmpiresMortarAimProjectileSpeed = 1800.0f;
	constexpr float kEmpiresMortarAimDefaultGravity = 800.0f;
	constexpr float kEmpiresMortarAimLaunchPitchOffset = 16.4f;
	constexpr float kEmpiresMortarLaunchForwardOffset = 14.0f;
	constexpr float kEmpiresMortarLaunchUpOffset = -3.0f;

	const char* empiresMortarClassName(const int team)
	{
		if (team == EMP_TEAM_NF)
			return "weapon_emp_nf_mortar";
		if (team == EMP_TEAM_BE)
			return "weapon_emp_imp_mortar";
		return nullptr;
	}

	const char* empiresMineClassName(const int team)
	{
		if (team == EMP_TEAM_NF)
			return "weapon_emp_nf_mine_grenade";
		if (team == EMP_TEAM_BE)
			return "weapon_emp_imp_mine_grenade";
		return nullptr;
	}

	const char* empiresMineProjectileClassName(const int team)
	{
		if (team == EMP_TEAM_NF)
			return "emp_nf_mine_grenade_projectile";
		if (team == EMP_TEAM_BE)
			return "emp_imp_mine_grenade_projectile";
		return nullptr;
	}

	int countEmpiresMortarShells(int* firstIndex = nullptr,
		const char** firstClassName = nullptr)
	{
		if (firstIndex != nullptr)
			*firstIndex = -1;
		if (firstClassName != nullptr)
			*firstClassName = nullptr;
		if (engine == nullptr || gpGlobals == nullptr)
			return 0;

		int count = 0;
		for (int index = gpGlobals->maxClients + 1;
			index <= gpGlobals->maxEntities; ++index)
		{
			edict_t* pEntity = engine->PEntityOfEntIndex(index);
			const char* className = pEntity != nullptr && !pEntity->IsFree() ?
				pEntity->GetClassName() : nullptr;
			if (className == nullptr ||
				(std::strcmp(className, "emp_shell") != 0 &&
					std::strcmp(className, "nf_mortar_shell") != 0 &&
					std::strcmp(className, "imp_mortar_shell") != 0))
				continue;

			if (firstIndex != nullptr && *firstIndex < 0)
				*firstIndex = index;
			if (firstClassName != nullptr && *firstClassName == nullptr)
				*firstClassName = className;
			++count;
		}
		return count;
	}

	int countEmpiresMineProjectiles(const int team, int* firstIndex = nullptr,
		const char** firstClassName = nullptr)
	{
		if (firstIndex != nullptr)
			*firstIndex = -1;
		if (firstClassName != nullptr)
			*firstClassName = nullptr;

		const char* expected = empiresMineProjectileClassName(team);
		if (expected == nullptr || engine == nullptr || gpGlobals == nullptr)
			return 0;

		int count = 0;
		for (int index = gpGlobals->maxClients + 1;
			index <= gpGlobals->maxEntities; ++index)
		{
			edict_t* pEntity = engine->PEntityOfEntIndex(index);
			const char* className = pEntity != nullptr && !pEntity->IsFree() ?
				pEntity->GetClassName() : nullptr;
			if (className == nullptr || std::strcmp(className, expected) != 0)
				continue;

			if (firstIndex != nullptr && *firstIndex < 0)
				*firstIndex = index;
			if (firstClassName != nullptr && *firstClassName == nullptr)
				*firstClassName = className;
			++count;
		}
		return count;
	}

	bool readEmpiresEntityVectorProperty(edict_t* pEdict, const char* propertyName,
		Vector& value)
	{
		if (pEdict == nullptr || propertyName == nullptr ||
			pEdict->GetUnknown() == nullptr ||
			pEdict->GetNetworkable() == nullptr)
			return false;

		CBaseEntity* pEntity = pEdict->GetUnknown()->GetBaseEntity();
		ServerClass* pServerClass = pEdict->GetNetworkable()->GetServerClass();
		unsigned offset = 0;
		if (pEntity == nullptr || pServerClass == nullptr ||
			!UTIL_FindSendPropInfo(pServerClass, propertyName, &offset) ||
			offset == 0)
			return false;

		const float* components = reinterpret_cast<const float*>(
			reinterpret_cast<const char*>(pEntity) + offset);
		value = Vector(components[0], components[1], components[2]);
		return value.IsValid();
	}
}

bool CBotEmpires::isEmpiresMortarActionActive() const
{
	return m_eEmpiresMortarActionState != EmpiresMortarActionState::IDLE &&
		m_eEmpiresMortarActionState != EmpiresMortarActionState::COMPLETE &&
		m_eEmpiresMortarActionState != EmpiresMortarActionState::FAILED;
}







bool CBotEmpires::isEmpiresMineActionActive() const
{
	return m_eEmpiresMineActionState != EmpiresMineActionState::IDLE &&
		m_eEmpiresMineActionState != EmpiresMineActionState::COMPLETE &&
		m_eEmpiresMineActionState != EmpiresMineActionState::FAILED;
}

void CBotEmpires::failEmpiresMineAction(const char* reason)
{
	if (!isEmpiresMineActionActive())
		return;

	if (m_pButtons != nullptr)
	{
		m_pButtons->letGo(IN_ATTACK);
		m_pButtons->letGo(IN_ATTACK2);
		m_pButtons->letGo(IN_RELOAD);
		m_pButtons->letGo(IN_DUCK);
		m_pButtons->letGo(IN_FORWARD);
		m_pButtons->letGo(IN_BACK);
		m_pButtons->letGo(IN_MOVELEFT);
		m_pButtons->letGo(IN_MOVERIGHT);
	}
	m_fForwardSpeed = 0.0f;
	m_fSideSpeed = 0.0f;
	m_fUpSpeed = 0.0f;
	m_iButtons = 0;
	wantToShoot(false);

	if (logger != nullptr)
	{
		logger->Log(LogLevel::WARN,
			"Empires mine action=result=FAIL index=%d userid=%d team=%d projectile_baseline=%d projectile_index=%d ammo_before=%d ammo_after=%d frames=%d reason=%s",
			engine != nullptr && m_pEdict != nullptr ? engine->IndexOfEdict(m_pEdict) : -1,
			engine != nullptr && m_pEdict != nullptr ? engine->GetPlayerUserId(m_pEdict) : -1,
			m_iEmpiresMineActionTeam, m_iEmpiresMineActionProjectileBaseline,
			m_iEmpiresMineActionProjectileIndex, m_iEmpiresMineActionAmmoBefore,
			m_iEmpiresMineActionAmmoAfter, m_iEmpiresMineActionFrames,
			reason != nullptr ? reason : "unspecified");
	}
	m_eEmpiresMineActionState = EmpiresMineActionState::FAILED;
}

void CBotEmpires::handleEmpiresMineAction()
{
	if (!isEmpiresMineActionActive())
		return;

	if (engine == nullptr || gpGlobals == nullptr || m_pEdict == nullptr ||
		m_pEdict->IsFree() || m_pPlayerInfo == nullptr ||
		m_pController == nullptr || m_pButtons == nullptr)
	{
		failEmpiresMineAction("runtime-prerequisite-invalid");
		return;
	}

	++m_iEmpiresMineActionFrames;
	const float now = gpGlobals->curtime;
	const int index = engine->IndexOfEdict(m_pEdict);
	const int userId = engine->GetPlayerUserId(m_pEdict);
	if (index != m_iEmpiresMineActionExpectedIndex ||
		userId != m_iEmpiresMineActionExpectedUserId ||
		m_pEdict != m_pEmpiresMineActionExpectedEdict)
	{
		failEmpiresMineAction("client-identity-changed");
		return;
	}

	const bool triggerDeathObservation =
		m_eEmpiresMineActionState == EmpiresMineActionState::OBSERVING_TRIGGER &&
		m_bEmpiresMineActionTriggerMode &&
		m_bEmpiresMineActionTriggerNaturalDeathLogged;
	if ((!triggerDeathObservation &&
			(m_pPlayerInfo->IsDead() || m_pPlayerInfo->GetHealth() <= 0)) ||
		m_pPlayerInfo->GetTeamIndex() != m_iEmpiresMineActionTeam)
	{
		failEmpiresMineAction("bot-no-longer-alive-or-team-changed");
		return;
	}

	const char* expectedClassName =
		empiresMineClassName(m_iEmpiresMineActionTeam);
	const edict_t* pCurrentWeapon = CClassInterface::getCurrentWeapon(m_pEdict);
	const bool mineActive = pCurrentWeapon != nullptr &&
		!pCurrentWeapon->IsFree() && pCurrentWeapon->GetClassName() != nullptr &&
		std::strcmp(pCurrentWeapon->GetClassName(), expectedClassName) == 0;

	// This one-shot action owns no movement or secondary input.  It does not
	// force crouch, aim, or any game state; the mine is tested through the
	// ordinary primary-input path only.
	m_fForwardSpeed = 0.0f;
	m_fSideSpeed = 0.0f;
	m_fUpSpeed = 0.0f;
	m_iButtons &= ~(IN_ATTACK | IN_ATTACK2 | IN_RELOAD | IN_FORWARD | IN_BACK |
		IN_MOVELEFT | IN_MOVERIGHT | IN_JUMP | IN_USE | IN_SPEED | IN_WALK | IN_DUCK);

	if (m_eEmpiresMineActionState == EmpiresMineActionState::WAITING_FOR_ACTIVE)
	{
		if (!mineActive)
		{
			selectWeaponName(expectedClassName);
			if (now >= m_flEmpiresMineActionDeadline)
				failEmpiresMineAction("mine-selection-timeout");
			return;
		}

		m_pEmpiresMineActionWeapon = pCurrentWeapon;
		m_eEmpiresMineActionState = EmpiresMineActionState::WAITING_FOR_READY;
		if (logger != nullptr)
			logger->Log(LogLevel::INFO,
				"Empires mine action=selected index=%d userid=%d active_weapon=%s weapon_index=%d",
				index, userId, pCurrentWeapon->GetClassName(),
				engine->IndexOfEdict(const_cast<edict_t*>(pCurrentWeapon)));
		return;
	}

	if (!mineActive && !triggerDeathObservation)
	{
		failEmpiresMineAction("mine-no-longer-active");
		return;
	}

	m_pEmpiresMineActionWeapon = pCurrentWeapon;
	edict_t* pWeapon = const_cast<edict_t*>(pCurrentWeapon);
	if (m_eEmpiresMineActionState == EmpiresMineActionState::WAITING_FOR_READY)
	{
		float playerNextAttack = 0.0f;
		const bool playerNextKnown = readEmpiresDebugFloatProperty(m_pEdict,
			"m_flNextAttack", playerNextAttack);
		float nextPrimaryAttack = 0.0f;
		const bool primaryNextKnown = readEmpiresWeaponFloatProperty(pWeapon,
			"m_flNextPrimaryAttack", nextPrimaryAttack);
		const int flags = CClassInterface::getPlayerFlags(m_pEdict);
		const bool onGround = (flags & FL_ONGROUND) != 0;
		const bool timersReady = (!playerNextKnown || playerNextAttack <= now) &&
			(!primaryNextKnown || nextPrimaryAttack <= now);

		if (timersReady)
		{
			if (logger != nullptr)
				logger->Log(LogLevel::INFO,
					"Empires mine action=ready index=%d userid=%d curtime=%.3f flags=0x%x fl_onground=%d player_next_known=%d player_next_attack=%.3f primary_next_known=%d next_primary=%.3f",
					index, userId, now, flags, onGround ? 1 : 0,
					playerNextKnown ? 1 : 0, playerNextAttack,
					primaryNextKnown ? 1 : 0, nextPrimaryAttack);
			m_pButtons->letGo(IN_ATTACK);
			m_pButtons->tap(IN_ATTACK);
			m_eEmpiresMineActionState = EmpiresMineActionState::ATTACK_SUBMITTED;
			m_flEmpiresMineActionProjectileDeadline = now + 4.0f;
			return;
		}

		if (now >= m_flEmpiresMineActionDeadline)
			failEmpiresMineAction("mine-readiness-timeout");
		return;
	}

	if (m_eEmpiresMineActionState == EmpiresMineActionState::ATTACK_SUBMITTED)
	{
		m_iButtons &= ~IN_ATTACK;
		m_pButtons->letGo(IN_ATTACK);
		m_eEmpiresMineActionState = EmpiresMineActionState::OBSERVING_RESULT;
		if (logger != nullptr)
			logger->Log(LogLevel::INFO,
				"Empires mine action=attack-release-armed index=%d userid=%d curtime=%.3f",
				index, userId, now);
		return;
	}

	if (m_eEmpiresMineActionState == EmpiresMineActionState::OBSERVING_RESULT)
	{
		m_iButtons &= ~IN_ATTACK;
		m_pButtons->letGo(IN_ATTACK);
		int ammoType1 = -1;
		int ammoType2 = -1;
		CClassInterface::getAmmoTypes(pWeapon, &ammoType1, &ammoType2);
		m_iEmpiresMineActionAmmoAfter = ammoType1 >= 0 ?
			getAmmo(static_cast<std::size_t>(ammoType1)) : -1;

		int projectileIndex = -1;
		const char* projectileClassName = nullptr;
		const int projectileCount = countEmpiresMineProjectiles(
			m_iEmpiresMineActionTeam, &projectileIndex, &projectileClassName);
		if (projectileCount > m_iEmpiresMineActionProjectileBaseline)
		{
			m_iEmpiresMineActionProjectileIndex = projectileIndex;
			if (logger != nullptr)
				logger->Log(LogLevel::INFO,
					"Empires mine action=result=PASS index=%d userid=%d team=%d projectile_created=1 projectile_class=%s projectile_index=%d projectile_count=%d baseline=%d ammo_before=%d ammo_after=%d",
					index, userId, m_iEmpiresMineActionTeam,
					projectileClassName != nullptr ? projectileClassName : "<none>",
					projectileIndex, projectileCount,
					m_iEmpiresMineActionProjectileBaseline,
					m_iEmpiresMineActionAmmoBefore,
					m_iEmpiresMineActionAmmoAfter);
			m_pButtons->letGo(IN_ATTACK);
			m_pButtons->letGo(IN_ATTACK2);
			m_pButtons->letGo(IN_RELOAD);
			m_iButtons = 0;
			m_fForwardSpeed = 0.0f;
			m_fSideSpeed = 0.0f;
			m_fUpSpeed = 0.0f;
			wantToShoot(false);
			m_eEmpiresMineActionState = EmpiresMineActionState::OBSERVING_DEPLOY;
			m_flEmpiresMineActionProjectileDeadline = now + 4.0f;
			return;
		}

		if (now >= m_flEmpiresMineActionProjectileDeadline)
			failEmpiresMineAction("no-mine-projectile-created");
		return;
	}

	if (m_eEmpiresMineActionState == EmpiresMineActionState::OBSERVING_DEPLOY)
	{
		m_iButtons &= ~IN_ATTACK;
		m_pButtons->letGo(IN_ATTACK);

		edict_t* pProjectile = m_iEmpiresMineActionProjectileIndex >= 0 ?
			engine->PEntityOfEntIndex(m_iEmpiresMineActionProjectileIndex) : nullptr;
		const char* expectedProjectile =
			empiresMineProjectileClassName(m_iEmpiresMineActionTeam);
		const bool projectileValid = pProjectile != nullptr &&
			!pProjectile->IsFree() && pProjectile->GetClassName() != nullptr &&
			expectedProjectile != nullptr &&
			std::strcmp(pProjectile->GetClassName(), expectedProjectile) == 0;
		if (!projectileValid)
		{
			if (now >= m_flEmpiresMineActionProjectileDeadline)
				failEmpiresMineAction("mine-projectile-removed-before-deploy-observation");
			return;
		}

		const bool moveTypeKnown =
			CClassInterface::getOffset(GETPROP_MOVETYPE) != 0;
		const int moveType = moveTypeKnown ?
			CClassInterface::getMoveType(pProjectile) : -1;
		const ICollideable* pCollideable = pProjectile->GetCollideable();
		const bool solidFlagsKnown = pCollideable != nullptr;
		const int solidFlags = solidFlagsKnown ?
			pCollideable->GetSolidFlags() : -1;
		Vector collisionOrigin;
		bool collisionOriginKnown = false;
		if (pCollideable != nullptr)
		{
			collisionOrigin = pCollideable->GetCollisionOrigin();
			collisionOriginKnown = collisionOrigin.IsValid();
		}
		// Empires mine projectiles do not expose valid IEntityInfo transform
		// telemetry.  Querying IEntityInfo here can return ABI-invalid data
		// (including NaN/garbage) and is not needed for the deploy proof.  Keep
		// the diagnostic values deterministic and rely on the dynamic collision
		// signature below, which is the supported planted-mine observation.
		Vector origin = vec3_origin;
		Vector velocity = vec3_origin;
		bool originKnown = false;
		bool velocityKnown = false;
		bool groundKnown = false;
		bool grounded = false;

		// EntityInfo exposes player telemetry reliably, but this current
		// Empires projectile has no EntityInfo origin/velocity/ground record.
		// Its dynamic collision state is sufficient: DeployThink makes it
		// stationary and turns it into a trigger/non-solid entity.
		const bool plantedSignature = moveTypeKnown && moveType == MOVETYPE_NONE &&
			solidFlagsKnown && (solidFlags & FSOLID_TRIGGER) != 0 &&
			(solidFlags & FSOLID_NOT_SOLID) != 0;
		if (plantedSignature)
			++m_iEmpiresMineActionSettledFrames;
		else
			m_iEmpiresMineActionSettledFrames = 0;
		const bool settled = plantedSignature &&
			m_iEmpiresMineActionSettledFrames >= 2;
		++m_iEmpiresMineActionDeployObservations;
		if (logger != nullptr &&
			(m_iEmpiresMineActionDeployObservations == 1 ||
				m_iEmpiresMineActionDeployObservations % 30 == 0))
		{
			logger->Log(LogLevel::INFO,
				"Empires mine action=deploy-observe index=%d userid=%d team=%d projectile_index=%d observation=%d move_type=%d move_type_known=%d ground=%d ground_known=%d solid_flags=%d solid_flags_known=%d velocity_known=%d velocity=(%.1f %.1f %.1f) origin_known=%d origin=(%.1f %.1f %.1f) planted_signature=%d settled_frames=%d settled=%d",
				index, userId, m_iEmpiresMineActionTeam,
				m_iEmpiresMineActionProjectileIndex,
				m_iEmpiresMineActionDeployObservations, moveType,
				moveTypeKnown ? 1 : 0, grounded ? 1 : 0,
				groundKnown ? 1 : 0, solidFlags, solidFlagsKnown ? 1 : 0,
				velocityKnown ? 1 : 0, velocity.x, velocity.y, velocity.z,
				originKnown ? 1 : 0, origin.x, origin.y, origin.z,
				plantedSignature ? 1 : 0,
				m_iEmpiresMineActionSettledFrames, settled ? 1 : 0);
		}
		if (settled)
		{
			bool activated = false;
			const bool activationKnown = readEmpiresEntityBoolProperty(
				pProjectile, "m_bActivated", activated);
			m_iEmpiresMineActionActivationLast = activationKnown ?
				(activated ? 1 : 0) : -1;
			m_bEmpiresMineActionActivationUnavailableLogged = false;
			m_bEmpiresMineActionActivationLogged = false;
			m_flEmpiresMineActionDeployedTime = now;
			EmpiresAnalysis::RecordBotEvent(this, "GRENADIER_MINE_PLACED",
				pProjectile, "deployed-state-observed");
			if (logger != nullptr)
			{
				logger->Log(LogLevel::INFO,
					"Empires mine action=deployed result=PASS index=%d userid=%d team=%d projectile_index=%d move_type=%d move_type_known=%d ground=%d ground_known=%d solid_flags=%d solid_flags_known=%d velocity_known=%d velocity=(%.1f %.1f %.1f) origin_known=%d origin=(%.1f %.1f %.1f) activation_known=%d activated=%d property=m_bActivated",
					index, userId, m_iEmpiresMineActionTeam,
					m_iEmpiresMineActionProjectileIndex, moveType,
					moveTypeKnown ? 1 : 0, grounded ? 1 : 0,
					groundKnown ? 1 : 0, solidFlags, solidFlagsKnown ? 1 : 0,
					velocityKnown ? 1 : 0, velocity.x, velocity.y, velocity.z,
					originKnown ? 1 : 0, origin.x, origin.y, origin.z,
					activationKnown ? 1 : 0, activationKnown && activated ? 1 : 0);
			}
			m_bEmpiresMineActionDeployLogged = true;
			if (m_bEmpiresMineActionTriggerMode)
			{
				edict_t* pTargetEdict = CBotGlobals::playerByUserId(
					m_iEmpiresMineActionTriggerTargetUserId);
				CBot* pTargetBase = pTargetEdict != nullptr ?
					CBots::getBotPointer(pTargetEdict) : nullptr;
				CBotEmpires* pTargetBot = pTargetBase != nullptr ?
					static_cast<CBotEmpires*>(pTargetBase) : nullptr;
				const bool targetIdentityValid = pTargetBot != nullptr &&
					pTargetEdict == m_pEmpiresMineActionTriggerTargetEdict &&
					!pTargetEdict->IsFree() &&
					engine->IndexOfEdict(pTargetEdict) ==
					m_iEmpiresMineActionTriggerTargetIndex &&
					engine->GetPlayerUserId(pTargetEdict) ==
					m_iEmpiresMineActionTriggerTargetUserId &&
					pTargetBot->getController() != nullptr;
				if (!targetIdentityValid || !collisionOriginKnown)
				{
					if (now >= m_flEmpiresMineActionProjectileDeadline)
						failEmpiresMineAction(!targetIdentityValid ?
							"mine-trigger-target-invalid" :
							"mine-trigger-collision-origin-unavailable");
					return;
				}

				m_eEmpiresMineActionState =
					EmpiresMineActionState::OBSERVING_TRIGGER;
				m_flEmpiresMineActionProjectileDeadline = now +
					m_flEmpiresMineActionTriggerDelaySeconds + 6.0f;
				if (m_flEmpiresMineActionTriggerDelaySeconds > 0.0f)
				{
					if (logger != nullptr)
						logger->Log(LogLevel::INFO,
							"Empires mine trigger=target-hold-start index=%d userid=%d team=%d projectile_index=%d target_userid=%d hold_seconds=%.3f reason=activation-delay-observation",
							index, userId, m_iEmpiresMineActionTeam,
							m_iEmpiresMineActionProjectileIndex,
							m_iEmpiresMineActionTriggerTargetUserId,
							m_flEmpiresMineActionTriggerDelaySeconds);
					return;
				}

				Vector targetOrigin = collisionOrigin;
				pTargetBot->getController()->SetAbsOrigin(targetOrigin);
				m_bEmpiresMineActionTriggerPositionLogged = true;
				if (logger != nullptr)
				{
					logger->Log(LogLevel::INFO,
						"Empires mine trigger=target-positioned index=%d userid=%d team=%d projectile_index=%d target_index=%d target_userid=%d mine_origin=(%.1f %.1f %.1f) target_origin=(%.1f %.1f %.1f) separation=%.1f method=ICollideable::GetCollisionOrigin",
						index, userId, m_iEmpiresMineActionTeam,
						m_iEmpiresMineActionProjectileIndex,
						m_iEmpiresMineActionTriggerTargetIndex,
						m_iEmpiresMineActionTriggerTargetUserId,
						collisionOrigin.x, collisionOrigin.y, collisionOrigin.z,
						targetOrigin.x, targetOrigin.y, targetOrigin.z,
						(collisionOrigin - targetOrigin).Length());
				}
				return;
			}
			m_pButtons->letGo(IN_ATTACK);
			m_pButtons->letGo(IN_ATTACK2);
			m_pButtons->letGo(IN_RELOAD);
			m_iButtons = 0;
			m_fForwardSpeed = 0.0f;
			m_fSideSpeed = 0.0f;
			m_fUpSpeed = 0.0f;
			wantToShoot(false);
			m_eEmpiresMineActionState = EmpiresMineActionState::COMPLETE;
			return;
		}

		if (now >= m_flEmpiresMineActionProjectileDeadline)
			failEmpiresMineAction("mine-projectile-did-not-reach-deployed-state");
		return;
	}

	if (m_eEmpiresMineActionState == EmpiresMineActionState::OBSERVING_TRIGGER)
	{
		m_iButtons &= ~IN_ATTACK;
		m_pButtons->letGo(IN_ATTACK);

		edict_t* pMineProjectile = m_iEmpiresMineActionProjectileIndex >= 0 ?
			engine->PEntityOfEntIndex(m_iEmpiresMineActionProjectileIndex) : nullptr;
		const char* expectedMineProjectile =
			empiresMineProjectileClassName(m_iEmpiresMineActionTeam);
		const bool mineProjectileValid = pMineProjectile != nullptr &&
			!pMineProjectile->IsFree() && pMineProjectile->GetClassName() != nullptr &&
			expectedMineProjectile != nullptr &&
			std::strcmp(pMineProjectile->GetClassName(), expectedMineProjectile) == 0;
		if (mineProjectileValid)
		{
			bool activated = false;
			const bool activationKnown = readEmpiresEntityBoolProperty(
				pMineProjectile, "m_bActivated", activated);
			if (activationKnown)
			{
				const int activationState = activated ? 1 : 0;
				if (m_iEmpiresMineActionActivationLast != activationState)
				{
					if (logger != nullptr)
						logger->Log(LogLevel::INFO,
							"Empires mine activation=transition index=%d userid=%d team=%d projectile_index=%d activated=%d elapsed_since_deploy=%.3f property=m_bActivated",
							engine->IndexOfEdict(m_pEdict),
							engine->GetPlayerUserId(m_pEdict),
							m_iEmpiresMineActionTeam,
							m_iEmpiresMineActionProjectileIndex,
							activationState,
							now - m_flEmpiresMineActionDeployedTime);
					m_iEmpiresMineActionActivationLast = activationState;
				}
				if (activationState == 1 &&
					!m_bEmpiresMineActionActivationLogged)
				{
					m_bEmpiresMineActionActivationLogged = true;
					if (logger != nullptr)
						logger->Log(LogLevel::INFO,
							"Empires mine activation=result=ARMED index=%d userid=%d team=%d projectile_index=%d elapsed_since_deploy=%.3f property=m_bActivated",
							engine->IndexOfEdict(m_pEdict),
							engine->GetPlayerUserId(m_pEdict),
							m_iEmpiresMineActionTeam,
							m_iEmpiresMineActionProjectileIndex,
							now - m_flEmpiresMineActionDeployedTime);
				}
			}
			else if (!m_bEmpiresMineActionActivationUnavailableLogged)
			{
				m_bEmpiresMineActionActivationUnavailableLogged = true;
				if (logger != nullptr)
					logger->Log(LogLevel::WARN,
						"Empires mine activation=UNKNOWN index=%d userid=%d team=%d projectile_index=%d property=m_bActivated unavailable=1",
						engine->IndexOfEdict(m_pEdict),
						engine->GetPlayerUserId(m_pEdict),
						m_iEmpiresMineActionTeam,
						m_iEmpiresMineActionProjectileIndex);
			}
		}

		edict_t* pTargetEdict = CBotGlobals::playerByUserId(
			m_iEmpiresMineActionTriggerTargetUserId);
		CBot* pTargetBase = pTargetEdict != nullptr ?
			CBots::getBotPointer(pTargetEdict) : nullptr;
		CBotEmpires* pTargetBot = pTargetBase != nullptr ?
			static_cast<CBotEmpires*>(pTargetBase) : nullptr;
		const bool targetIdentityValid = pTargetBot != nullptr &&
			pTargetEdict == m_pEmpiresMineActionTriggerTargetEdict &&
			!pTargetEdict->IsFree() &&
			engine->IndexOfEdict(pTargetEdict) ==
			m_iEmpiresMineActionTriggerTargetIndex &&
			engine->GetPlayerUserId(pTargetEdict) ==
			m_iEmpiresMineActionTriggerTargetUserId &&
			pTargetBot->getController() != nullptr;
		if (!targetIdentityValid)
		{
			if (now >= m_flEmpiresMineActionProjectileDeadline)
				failEmpiresMineAction("mine-trigger-target-invalid");
			return;
		}

		IPlayerInfo* pTargetInfo = playerinfomanager->GetPlayerInfo(pTargetEdict);
		const int targetHealth = pTargetInfo != nullptr ?
			pTargetInfo->GetHealth() : -1;
		const bool targetDamaged = targetHealth >= 0 &&
			m_iEmpiresMineActionTriggerTargetHealthBefore >= 0 &&
			targetHealth < m_iEmpiresMineActionTriggerTargetHealthBefore;
		const bool targetDead = pTargetInfo != nullptr &&
			(pTargetInfo->IsDead() || targetHealth <= 0);
		if ((targetDamaged || targetDead) &&
			!m_bEmpiresMineActionTriggerImpactLogged)
		{
			m_bEmpiresMineActionTriggerImpactLogged = true;
			if (logger != nullptr)
				logger->Log(LogLevel::INFO,
					"Empires mine trigger=target-affected index=%d userid=%d target_index=%d target_userid=%d health_before=%d health_after=%d target_dead=%d",
					engine->IndexOfEdict(m_pEdict),
					engine->GetPlayerUserId(m_pEdict),
					m_iEmpiresMineActionTriggerTargetIndex,
					m_iEmpiresMineActionTriggerTargetUserId,
					m_iEmpiresMineActionTriggerTargetHealthBefore,
					targetHealth, targetDead ? 1 : 0);
		}

		edict_t* pProjectile = m_iEmpiresMineActionProjectileIndex >= 0 ?
			engine->PEntityOfEntIndex(m_iEmpiresMineActionProjectileIndex) : nullptr;
		const char* expectedProjectile =
			empiresMineProjectileClassName(m_iEmpiresMineActionTeam);
		const bool projectileValid = pProjectile != nullptr &&
			!pProjectile->IsFree() && pProjectile->GetClassName() != nullptr &&
			expectedProjectile != nullptr &&
			std::strcmp(pProjectile->GetClassName(), expectedProjectile) == 0;
		if (!projectileValid)
		{
			const bool targetAffected = targetDamaged || targetDead ||
				m_bEmpiresMineActionTriggerImpactLogged;
			const bool targetWasDead = targetDead ||
				m_bEmpiresMineActionTriggerNaturalDeathLogged;
			const int reportedTargetHealth =
				m_bEmpiresMineActionTriggerNaturalDeathLogged &&
				m_iEmpiresMineActionTriggerTargetHealthAfter != -1 ?
				m_iEmpiresMineActionTriggerTargetHealthAfter : targetHealth;
			if (targetAffected)
			{
				EmpiresAnalysis::RecordBotEvent(this,
					"GRENADIER_MINE_DETONATED", pTargetEdict,
					"engine-detonation-observed");
				if (logger != nullptr)
					logger->Log(LogLevel::INFO,
						"Empires mine trigger=result=PASS index=%d userid=%d team=%d projectile_index=%d projectile_removed=1 target_index=%d target_userid=%d health_before=%d health_after=%d target_dead=%d reason=engine-detonation-observed",
						engine->IndexOfEdict(m_pEdict),
						engine->GetPlayerUserId(m_pEdict),
						m_iEmpiresMineActionTeam,
						m_iEmpiresMineActionProjectileIndex,
						m_iEmpiresMineActionTriggerTargetIndex,
						m_iEmpiresMineActionTriggerTargetUserId,
						m_iEmpiresMineActionTriggerTargetHealthBefore,
						reportedTargetHealth, targetWasDead ? 1 : 0);
				m_iButtons = 0;
				m_fForwardSpeed = 0.0f;
				m_fSideSpeed = 0.0f;
				m_fUpSpeed = 0.0f;
				m_pButtons->letGo(IN_ATTACK);
				m_pButtons->letGo(IN_ATTACK2);
				m_pButtons->letGo(IN_RELOAD);
				m_pButtons->letGo(IN_DUCK);
				wantToShoot(false);
				m_bEmpiresMineActionTriggerMode = false;
				m_eEmpiresMineActionState = EmpiresMineActionState::COMPLETE;
			}
			else if (now >= m_flEmpiresMineActionProjectileDeadline)
				failEmpiresMineAction("mine-projectile-removed-without-target-damage");
			return;
		}

		// This placement is restricted to the debug fixture. Keeping the target
		// at the real collision origin makes the normal Empires mine think path
		// responsible for the trigger; no damage or projectile state is injected.
		const bool triggerDelayElapsed = now >=
			m_flEmpiresMineActionDeployedTime +
			m_flEmpiresMineActionTriggerDelaySeconds;
		if (!m_bEmpiresMineActionTriggerPositionLogged &&
			!triggerDelayElapsed)
		{
			if (!m_bEmpiresMineActionTriggerHoldLogged)
			{
				m_bEmpiresMineActionTriggerHoldLogged = true;
				if (logger != nullptr)
					logger->Log(LogLevel::INFO,
						"Empires mine trigger=target-hold-observe index=%d userid=%d team=%d projectile_index=%d elapsed_since_deploy=%.3f remaining=%.3f projectile_alive=1",
						engine->IndexOfEdict(m_pEdict),
						engine->GetPlayerUserId(m_pEdict),
						m_iEmpiresMineActionTeam,
						m_iEmpiresMineActionProjectileIndex,
						now - m_flEmpiresMineActionDeployedTime,
						m_flEmpiresMineActionDeployedTime +
						m_flEmpiresMineActionTriggerDelaySeconds - now);
			}
			return;
		}
		if (!m_bEmpiresMineActionTriggerPositionLogged &&
			pTargetInfo != nullptr && !pTargetInfo->IsDead() &&
			targetHealth > 0)
		{
			const ICollideable* pCollideable = pProjectile->GetCollideable();
			if (pCollideable != nullptr)
			{
				Vector mineOrigin = pCollideable->GetCollisionOrigin();
				if (mineOrigin.IsValid())
				{
					Vector targetOrigin = mineOrigin;
					pTargetBot->getController()->SetAbsOrigin(targetOrigin);
					m_bEmpiresMineActionTriggerPositionLogged = true;
					if (logger != nullptr)
						logger->Log(LogLevel::INFO,
							"Empires mine trigger=target-positioned index=%d userid=%d team=%d projectile_index=%d target_index=%d target_userid=%d mine_origin=(%.1f %.1f %.1f) target_origin=(%.1f %.1f %.1f) separation=%.1f method=ICollideable::GetCollisionOrigin delayed_hold_seconds=%.3f elapsed_since_deploy=%.3f",
							engine->IndexOfEdict(m_pEdict),
							engine->GetPlayerUserId(m_pEdict),
							m_iEmpiresMineActionTeam,
							m_iEmpiresMineActionProjectileIndex,
							m_iEmpiresMineActionTriggerTargetIndex,
							m_iEmpiresMineActionTriggerTargetUserId,
							mineOrigin.x, mineOrigin.y, mineOrigin.z,
							targetOrigin.x, targetOrigin.y, targetOrigin.z,
							(mineOrigin - targetOrigin).Length(),
							m_flEmpiresMineActionTriggerDelaySeconds,
							now - m_flEmpiresMineActionDeployedTime);
				}
			}
		}

		if (now >= m_flEmpiresMineActionProjectileDeadline)
			failEmpiresMineAction("mine-trigger-timeout");
	}
}
void CBotEmpires::failEmpiresMortarAction(const char* reason)
{
	if (!isEmpiresMortarActionActive())
		return;

	if (m_pButtons != nullptr)
	{
		m_pButtons->letGo(IN_ATTACK);
		m_pButtons->letGo(IN_ATTACK2);
		m_pButtons->letGo(IN_RELOAD);
		m_pButtons->letGo(IN_DUCK);
	}
	m_fForwardSpeed = 0.0f;
	m_fSideSpeed = 0.0f;
	m_fUpSpeed = 0.0f;
	m_iButtons = 0;
	wantToShoot(false);

	if (logger != nullptr)
	{
		// Ammo and health tell a shell that never left from one that burst at once.
		int ammoAfter = -1, ammoType1 = -1, ammoType2 = -1;
		edict_t* pWeapon = m_pEdict != nullptr ? CClassInterface::getCurrentWeapon(m_pEdict) : nullptr;
		if (pWeapon != nullptr && !pWeapon->IsFree())
		{
			CClassInterface::getAmmoTypes(pWeapon, &ammoType1, &ammoType2);
			if (ammoType1 >= 0)
				ammoAfter = getAmmo(static_cast<std::size_t>(ammoType1));
		}
		bool loaded = false;
		const bool loadedKnown = m_pEdict != nullptr && readEmpiresBoolProperty(m_pEdict, "m_bMortarLoaded", loaded);
		logger->Log(LogLevel::WARN,
			"Empires mortar action=result=FAIL team=%d index=%d userid=%d state=%d reason=%s frames=%d shell_baseline=%d ammo_before=%d ammo_now=%d loaded=%d health=%d",
			m_iEmpiresMortarActionTeam,
			engine != nullptr && m_pEdict != nullptr ? engine->IndexOfEdict(m_pEdict) : -1,
			engine != nullptr && m_pEdict != nullptr ? engine->GetPlayerUserId(m_pEdict) : -1,
			static_cast<int>(m_eEmpiresMortarActionState),
			reason != nullptr ? reason : "unspecified",
			m_iEmpiresMortarActionFrames, m_iEmpiresMortarActionShellBaseline,
			m_iEmpiresMortarActionAmmoBefore, ammoAfter, loadedKnown ? (loaded ? 1 : 0) : -1,
			m_pPlayerInfo != nullptr ? m_pPlayerInfo->GetHealth() : -1);
	}
	m_eEmpiresMortarActionState = EmpiresMortarActionState::FAILED;
}

namespace
{
	class CTraceFilterMortarArc : public CTraceFilter
	{
	public:
		bool ShouldHitEntity(IHandleEntity* entity, int) override
		{
			const int index = entity->GetRefEHandle().GetEntryIndex();
			const ICollideable* collision = enginetrace->GetCollideable(entity);
			return (index == 0 || index > gpGlobals->maxClients) &&
				(collision == nullptr || collision->GetCollisionGroup() != COLLISION_GROUP_VEHICLE_CLIP);
		}
	};

	float mortarPitchOffset(float viewPitch)
	{
		// PrimaryAttack tapers the offset as the player looks straight up.
		const float upward = std::clamp(-viewPitch / 89.0f, 0.0f, 1.0f);
		return kEmpiresMortarAimLaunchPitchOffset * (1.0f - std::pow(upward, 7.0f));
	}

	bool traceMortarArc(const Vector& launchOrigin, const Vector& velocity,
		float gravity, const Vector& target, Vector& hitPoint)
	{
		// Match the live shell's solid mask and two-unit collision extents.
		// A bullet ray can pass a grate or an opening that stops the shell.
		constexpr unsigned int kShellMask = 0x42c0608b;
		constexpr int kSteps = 40;
		const float flight = (target - launchOrigin).Length2D() / velocity.Length2D();
		Vector previous = launchOrigin;
		CTraceFilterMortarArc filter;
		for (int step = 1; step <= kSteps; ++step)
		{
			const float time = flight * step / kSteps;
			const Vector point = launchOrigin + velocity * time -
				Vector(0, 0, 0.5f * gravity * time * time);
			Ray_t ray;
			ray.Init(previous, point, Vector(-2, -2, -2), Vector(2, 2, 2));
			trace_t hit;
			enginetrace->TraceRay(ray, kShellMask, &filter, &hit);
			if (hit.DidHit())
			{
				hitPoint = hit.endpos;
				return !hit.startsolid && hitPoint.DistToSqr(target) < 200.0f * 200.0f;
			}
			previous = point;
		}
		return true;
	}
}

bool CBotEmpires::findEmpiresMortarAim(const Vector& eye, const Vector& target,
	bool checkArc, QAngle& angles, Vector& launchOrigin, Vector& hitPoint) const
{
	const float gravity = sv_gravity.IsValid() ? sv_gravity.GetFloat() :
		kEmpiresMortarAimDefaultGravity;
	hitPoint = eye;
	if (!eye.IsValid() || !target.IsValid() || !std::isfinite(gravity) ||
		gravity <= 0.0f || (checkArc && enginetrace == nullptr))
		return false;

	constexpr double speed = kEmpiresMortarAimProjectileSpeed;
	constexpr double speedSquared = speed * speed;
	for (int arc = 0; arc < (checkArc ? 2 : 1); ++arc)
	{
		launchOrigin = eye;
		float launchPitch = 0.0f;
		for (int iteration = 0; iteration < 3; ++iteration)
		{
			const Vector delta = target - launchOrigin;
			const double range = delta.Length2D();
			const double discriminant = speedSquared * speedSquared -
				gravity * (gravity * range * range + 2.0 * delta.z * speedSquared);
			if (range < 1.0 || !std::isfinite(discriminant) || discriminant < 0.0)
				return false;
			const double root = std::sqrt(discriminant);
			launchPitch = -RAD2DEG(std::atan((speedSquared + (arc == 0 ? -root : root)) /
				(gravity * range)));
			float viewPitch = launchPitch + kEmpiresMortarAimLaunchPitchOffset;
			for (int adjust = 0; adjust < 4; ++adjust)
				viewPitch = launchPitch + mortarPitchOffset(viewPitch);
			angles = QAngle(viewPitch, RAD2DEG(std::atan2(delta.y, delta.x)), 0.0f);

			Vector forward, up;
			AngleVectors(angles, &forward, nullptr, &up);
			launchOrigin = eye + forward * kEmpiresMortarLaunchForwardOffset +
				up * kEmpiresMortarLaunchUpOffset;
		}

		Vector forward;
		AngleVectors(QAngle(angles.x - mortarPitchOffset(angles.x), angles.y, 0.0f), &forward);
		if (!checkArc || traceMortarArc(launchOrigin,
			forward * kEmpiresMortarAimProjectileSpeed, gravity, target, hitPoint))
			return true;
	}
	return false;
}

void CBotEmpires::handleEmpiresMortarAction()
{
	if (!isEmpiresMortarActionActive())
		return;

	if (engine == nullptr || gpGlobals == nullptr || m_pEdict == nullptr ||
		m_pEdict->IsFree() || m_pPlayerInfo == nullptr || m_pController == nullptr ||
		m_pButtons == nullptr)
	{
		failEmpiresMortarAction("runtime-prerequisite-invalid");
		return;
	}

	++m_iEmpiresMortarActionFrames;
	const float now = gpGlobals->curtime;
	const int index = engine->IndexOfEdict(m_pEdict);
	const int userId = engine->GetPlayerUserId(m_pEdict);
	if (index != m_iEmpiresMortarActionExpectedIndex ||
		userId != m_iEmpiresMortarActionExpectedUserId ||
		m_pEdict != m_pEmpiresMortarActionExpectedEdict)
	{
		failEmpiresMortarAction("client-identity-changed");
		return;
	}

	if (m_pPlayerInfo->IsDead() || m_pPlayerInfo->GetHealth() <= 0 ||
		m_pPlayerInfo->GetTeamIndex() != m_iEmpiresMortarActionTeam)
	{
		failEmpiresMortarAction("bot-no-longer-alive-or-team-changed");
		return;
	}

	const char* expectedClassName =
		empiresMortarClassName(m_iEmpiresMortarActionTeam);
	const edict_t* pCurrentWeapon = CClassInterface::getCurrentWeapon(m_pEdict);
	const bool mortarActive = pCurrentWeapon != nullptr &&
		!pCurrentWeapon->IsFree() && pCurrentWeapon->GetClassName() != nullptr &&
		std::strcmp(pCurrentWeapon->GetClassName(), expectedClassName) == 0;

	// Mortar actions deliberately own no movement input.  This does not alter
	// the navigation task; it only prevents a movement command from being
	// combined with the one crouched firing command.
	m_fForwardSpeed = 0.0f;
	m_fSideSpeed = 0.0f;
	m_fUpSpeed = 0.0f;
	m_iButtons &= ~(IN_ATTACK | IN_ATTACK2 | IN_RELOAD | IN_FORWARD | IN_BACK |
		IN_MOVELEFT | IN_MOVERIGHT | IN_JUMP | IN_USE | IN_SPEED | IN_WALK);
	m_pButtons->duck(true);

	if (m_eEmpiresMortarActionState == EmpiresMortarActionState::WAITING_FOR_ACTIVE)
	{
		if (!mortarActive)
		{
			selectWeaponName(expectedClassName);
			if (now >= m_flEmpiresMortarActionDeadline)
				failEmpiresMortarAction("mortar-selection-timeout");
			return;
		}

		m_pEmpiresMortarActionWeapon = pCurrentWeapon;
		m_eEmpiresMortarActionState = EmpiresMortarActionState::WAITING_FOR_READY;
		if (logger != nullptr)
			logger->Log(LogLevel::INFO,
				"Empires mortar action=selected index=%d userid=%d active_weapon=%s weapon_index=%d",
				index, userId, pCurrentWeapon->GetClassName(),
				engine->IndexOfEdict(const_cast<edict_t*>(pCurrentWeapon)));
		return;
	}

	if (!mortarActive)
	{
		failEmpiresMortarAction("mortar-no-longer-active");
		return;
	}

	m_pEmpiresMortarActionWeapon = pCurrentWeapon;
	edict_t* pWeapon = const_cast<edict_t*>(pCurrentWeapon);
	const int flags = CClassInterface::getPlayerFlags(m_pEdict);
	const bool onGround = (flags & FL_ONGROUND) != 0;
	const bool ducking = (flags & FL_DUCKING) != 0;
	if (onGround && ducking && !m_bEmpiresMortarActionPostureLogged)
	{
		if (logger != nullptr)
			logger->Log(LogLevel::INFO,
				"Empires mortar action=posture-ready index=%d userid=%d flags=0x%x fl_onground=1 fl_ducking=1 fl_fakeclient=%d",
				index, userId, flags, m_pPlayerInfo->IsFakeClient() ? 1 : 0);
		m_bEmpiresMortarActionPostureLogged = true;
	}

	if (m_eEmpiresMortarActionState == EmpiresMortarActionState::WAITING_FOR_READY)
	{
		bool loaded = false;
	const bool loadedKnown = readEmpiresBoolProperty(m_pEdict,
			"m_bMortarLoaded", loaded);
		float reloadTime = 0.0f;
		const bool reloadKnown = readEmpiresWeaponFloatProperty(pWeapon,
			"m_flReloadTime", reloadTime);
		float playerNextAttack = 0.0f;
		const bool playerNextKnown = readEmpiresDebugFloatProperty(m_pEdict,
			"m_flNextAttack", playerNextAttack);
		float nextPrimaryAttack = 0.0f;
		const bool primaryNextKnown = readEmpiresWeaponFloatProperty(pWeapon,
			"m_flNextPrimaryAttack", nextPrimaryAttack);

		if (onGround && ducking && loadedKnown && loaded && reloadKnown &&
			reloadTime <= now && playerNextKnown && playerNextAttack <= now &&
			primaryNextKnown && nextPrimaryAttack <= now)
		{
			// SetAbsOrigin() in the debug fixture can leave the bot airborne for
			// a few frames even while the posture flags are already set.  Compute
			// immediately before the actual attack, after the runtime origin has
			// settled, so the solution matches PrimaryAttack's launch position.
			if (m_bEmpiresMortarAimRequested && !m_bEmpiresMortarAimComputed)
			{
				Vector hitPoint;
				if (!findEmpiresMortarAim(getEyePosition(), m_vEmpiresMortarAimTarget,
					m_bEmpiresMortarAimCheckArc, m_qEmpiresMortarAimAngles,
					m_vEmpiresMortarAimLaunchOrigin, hitPoint))
				{
					failEmpiresMortarAction(m_bEmpiresMortarAimCheckArc ?
						"mortar-arc-blocked" : "mortar-aim-calculation-failed");
					return;
				}

				m_bEmpiresMortarAimComputed = true;
				m_vViewAngles = m_qEmpiresMortarAimAngles;
				if (logger != nullptr)
					logger->Log(LogLevel::INFO,
						"Empires mortar aim=calculated index=%d userid=%d target_waypoint=%d target=(%.1f %.1f %.1f) launch=(%.1f %.1f %.1f) angles=(%.2f %.2f %.2f) launch_pitch=%.2f projectile_speed=%.1f",
						index, userId, m_iEmpiresMortarAimTargetWaypoint,
						m_vEmpiresMortarAimTarget.x, m_vEmpiresMortarAimTarget.y,
						m_vEmpiresMortarAimTarget.z, m_vEmpiresMortarAimLaunchOrigin.x,
						m_vEmpiresMortarAimLaunchOrigin.y,
						m_vEmpiresMortarAimLaunchOrigin.z,
						m_qEmpiresMortarAimAngles.x, m_qEmpiresMortarAimAngles.y,
						m_qEmpiresMortarAimAngles.z,
						m_qEmpiresMortarAimAngles.x - mortarPitchOffset(m_qEmpiresMortarAimAngles.x),
						kEmpiresMortarAimProjectileSpeed);
			}

			if (logger != nullptr && m_bEmpiresMortarAimRequested)
			{
				const CBotCmd lastCommand = m_pPlayerInfo->GetLastUserCommand();
				const QAngle serverEyeAngles = CBotGlobals::playerAngles(m_pEdict);
				const Vector runtimeOrigin = CBotGlobals::entityOrigin(m_pEdict);
				const Vector runtimeEye = getEyePosition();
				logger->Log(LogLevel::INFO,
					"Empires mortar aim=pre-attack-state index=%d userid=%d origin=(%.1f %.1f %.1f) eye=(%.1f %.1f %.1f) lastcmd_view=(%.2f %.2f %.2f) server_eye=(%.2f %.2f %.2f)",
					index, userId, runtimeOrigin.x, runtimeOrigin.y,
					runtimeOrigin.z, runtimeEye.x, runtimeEye.y, runtimeEye.z,
					lastCommand.viewangles.x, lastCommand.viewangles.y,
					lastCommand.viewangles.z, serverEyeAngles.x,
					serverEyeAngles.y, serverEyeAngles.z);
			}

			if (logger != nullptr)
				logger->Log(LogLevel::INFO,
					"Empires mortar action=ready index=%d userid=%d curtime=%.3f flags=0x%x fl_onground=1 fl_ducking=1 loaded=1 reload_time=%.3f player_next_attack=%.3f next_primary=%.3f",
					index, userId, now, flags, reloadTime, playerNextAttack,
					nextPrimaryAttack);
			m_pButtons->letGo(IN_ATTACK);
			m_pButtons->tap(IN_ATTACK);
			m_pButtons->duck(true);
			m_eEmpiresMortarActionState = EmpiresMortarActionState::ATTACK_SUBMITTED;
			EmpiresAnalysis::RecordBotEvent(this, "GRENADIER_MORTAR",
				m_pEnemy.get(), "normal-mortar-attack-submitted");
			m_flEmpiresMortarActionShellDeadline = now + 4.0f;
			return;
		}

		if (now >= m_flEmpiresMortarActionDeadline)
		{
			failEmpiresMortarAction(loadedKnown ?
				"mortar-readiness-timeout" : "mortar-property-unavailable");
		}
		return;
	}

	if (m_eEmpiresMortarActionState == EmpiresMortarActionState::ATTACK_SUBMITTED)
	{
		// The preceding tick submitted the one attack pulse.  Clearing only the
		// attack button here produces the required release command while duck is
		// kept by the normal button scheduler.
		m_iButtons &= ~IN_ATTACK;
		m_pButtons->letGo(IN_ATTACK);
		m_pButtons->duck(true);
		m_eEmpiresMortarActionState = EmpiresMortarActionState::OBSERVING_RESULT;
		if (logger != nullptr)
			logger->Log(LogLevel::INFO,
				"Empires mortar action=attack-release-armed index=%d userid=%d curtime=%.3f",
				index, userId, now);
		return;
	}

	if (m_eEmpiresMortarActionState == EmpiresMortarActionState::OBSERVING_TRAJECTORY)
	{
		m_iButtons &= ~IN_ATTACK;
		m_pButtons->letGo(IN_ATTACK);
		m_pButtons->duck(true);

		edict_t* pShell = m_iEmpiresMortarActionShellIndex > 0
			? engine->PEntityOfEntIndex(m_iEmpiresMortarActionShellIndex) : nullptr;
		if (pShell != nullptr && !pShell->IsFree() &&
			pShell->GetClassName() != nullptr &&
			std::strcmp(pShell->GetClassName(), "emp_shell") == 0)
		{
			const Vector shellOrigin = CBotGlobals::entityOrigin(pShell);
			const float sampleDelta = now - m_flEmpiresMortarAimLastSampleTime;
			if (sampleDelta >= 0.05f && m_iEmpiresMortarAimTrajectorySamples < 8)
			{
				const Vector displacement = shellOrigin -
					m_vEmpiresMortarAimLastShellOrigin;
				++m_iEmpiresMortarAimTrajectorySamples;
				if (logger != nullptr)
					logger->Log(LogLevel::INFO,
						"Empires mortar aim=trajectory-sample index=%d userid=%d sample=%d curtime=%.3f shell_origin=(%.1f %.1f %.1f) delta=%.3f estimated_speed=%.1f target_error=%.1f",
						index, userId, m_iEmpiresMortarAimTrajectorySamples, now,
						shellOrigin.x, shellOrigin.y, shellOrigin.z, sampleDelta,
						displacement.Length() / sampleDelta,
						(shellOrigin - m_vEmpiresMortarAimTarget).Length());
				m_vEmpiresMortarAimLastShellOrigin = shellOrigin;
				m_flEmpiresMortarAimLastSampleTime = now;
			}

			if (now < m_flEmpiresMortarActionShellDeadline)
				return;

			if (logger != nullptr)
				logger->Log(LogLevel::INFO,
					"Empires mortar aim=result=NO_IMPACT_OBSERVED index=%d userid=%d samples=%d last_origin=(%.1f %.1f %.1f) target=(%.1f %.1f %.1f) last_error=%.1f",
					index, userId, m_iEmpiresMortarAimTrajectorySamples,
					m_vEmpiresMortarAimLastShellOrigin.x,
					m_vEmpiresMortarAimLastShellOrigin.y,
					m_vEmpiresMortarAimLastShellOrigin.z,
					m_vEmpiresMortarAimTarget.x, m_vEmpiresMortarAimTarget.y,
					m_vEmpiresMortarAimTarget.z,
					(m_vEmpiresMortarAimLastShellOrigin -
						m_vEmpiresMortarAimTarget).Length());
		}
		else if (logger != nullptr)
		{
			logger->Log(LogLevel::INFO,
				"Empires mortar aim=result=IMPACT_OR_SHELL_REMOVED index=%d userid=%d samples=%d last_origin=(%.1f %.1f %.1f) target=(%.1f %.1f %.1f) last_error=%.1f",
				index, userId, m_iEmpiresMortarAimTrajectorySamples,
				m_vEmpiresMortarAimLastShellOrigin.x,
				m_vEmpiresMortarAimLastShellOrigin.y,
				m_vEmpiresMortarAimLastShellOrigin.z,
				m_vEmpiresMortarAimTarget.x, m_vEmpiresMortarAimTarget.y,
				m_vEmpiresMortarAimTarget.z,
				(m_vEmpiresMortarAimLastShellOrigin -
					m_vEmpiresMortarAimTarget).Length());
		}

		m_pButtons->letGo(IN_ATTACK2);
		m_pButtons->letGo(IN_RELOAD);
		m_pButtons->letGo(IN_DUCK);
		m_iButtons = 0;
		m_fForwardSpeed = 0.0f;
		m_fSideSpeed = 0.0f;
		m_fUpSpeed = 0.0f;
		wantToShoot(false);
		m_eEmpiresMortarActionState = EmpiresMortarActionState::COMPLETE;
		return;
	}

	if (m_eEmpiresMortarActionState == EmpiresMortarActionState::OBSERVING_RESULT)
	{
		m_iButtons &= ~IN_ATTACK;
		m_pButtons->letGo(IN_ATTACK);
		m_pButtons->duck(true);
		int shellIndex = -1;
		const char* shellClassName = nullptr;
		const int shellCount = countEmpiresMortarShells(&shellIndex,
			&shellClassName);
		int clip1 = -1;
		int clip2 = -1;
		int ammoType1 = -1;
		int ammoType2 = -1;
		CClassInterface::getWeaponClip(pWeapon, &clip1, &clip2);
		CClassInterface::getAmmoTypes(pWeapon, &ammoType1, &ammoType2);
		const int ammoAfter = ammoType1 >= 0 ?
			getAmmo(static_cast<std::size_t>(ammoType1)) : -1;
		m_iEmpiresMortarActionAmmoAfter = ammoAfter;

		if (shellCount > m_iEmpiresMortarActionShellBaseline)
		{
			bool loaded = false;
			const bool loadedKnown = readEmpiresBoolProperty(m_pEdict,
				"m_bMortarLoaded", loaded);
			float nextPrimaryAttack = 0.0f;
			const bool primaryNextKnown = readEmpiresWeaponFloatProperty(pWeapon,
				"m_flNextPrimaryAttack", nextPrimaryAttack);
			int shellHandleIndex = -1;
			const bool shellHandleKnown = readEmpiresWeaponHandleProperty(pWeapon,
				"m_hShell", shellHandleIndex);
			m_iEmpiresMortarActionShellIndex = shellIndex;
			Vector shellVelocity(0.0f, 0.0f, 0.0f);
			const bool shellVelocityKnown = shellIndex > 0 &&
				readEmpiresEntityVectorProperty(
					engine->PEntityOfEntIndex(shellIndex), "m_vecVelocity[0]",
					shellVelocity);
			if (logger != nullptr)
				logger->Log(LogLevel::INFO,
					"Empires mortar action=result=PASS index=%d userid=%d shell_created=1 shell_class=%s shell_index=%d shell_count=%d baseline=%d ammo_before=%d ammo_after=%d loaded_known=%d loaded=%d primary_known=%d next_primary=%.3f shell_handle_known=%d shell_handle_index=%d shell_velocity_known=%d shell_velocity=(%.1f %.1f %.1f) shell_speed=%.1f",
					index, userId, shellClassName != nullptr ? shellClassName : "<none>",
					shellIndex, shellCount, m_iEmpiresMortarActionShellBaseline,
					m_iEmpiresMortarActionAmmoBefore, ammoAfter, loadedKnown ? 1 : 0,
					loaded ? 1 : 0, primaryNextKnown ? 1 : 0, nextPrimaryAttack,
					shellHandleKnown ? 1 : 0, shellHandleIndex,
					shellVelocityKnown ? 1 : 0, shellVelocity.x, shellVelocity.y,
					shellVelocity.z, shellVelocity.Length());
			if (m_bEmpiresMortarAimRequested)
			{
				edict_t* pShell = engine->PEntityOfEntIndex(shellIndex);
				if (pShell == nullptr || pShell->IsFree())
				{
					failEmpiresMortarAction("mortar-aim-shell-disappeared-at-creation");
					return;
				}

				m_vEmpiresMortarAimFirstShellOrigin =
					CBotGlobals::entityOrigin(pShell);
				m_vEmpiresMortarAimLastShellOrigin =
					m_vEmpiresMortarAimFirstShellOrigin;
				m_iEmpiresMortarAimTrajectorySamples = 0;
				m_flEmpiresMortarAimLastSampleTime = now;
				m_flEmpiresMortarActionShellDeadline = now + 4.0f;
				m_eEmpiresMortarActionState =
					EmpiresMortarActionState::OBSERVING_TRAJECTORY;
				if (logger != nullptr)
					logger->Log(LogLevel::INFO,
						"Empires mortar aim=shell-created index=%d userid=%d shell_index=%d shell_origin=(%.1f %.1f %.1f) target=(%.1f %.1f %.1f)",
						index, userId, shellIndex,
						m_vEmpiresMortarAimFirstShellOrigin.x,
						m_vEmpiresMortarAimFirstShellOrigin.y,
						m_vEmpiresMortarAimFirstShellOrigin.z,
						m_vEmpiresMortarAimTarget.x,
						m_vEmpiresMortarAimTarget.y,
						m_vEmpiresMortarAimTarget.z);
				return;
			}
			m_pButtons->letGo(IN_ATTACK);
			m_pButtons->letGo(IN_ATTACK2);
			m_pButtons->letGo(IN_RELOAD);
			m_pButtons->letGo(IN_DUCK);
			m_iButtons = 0;
			m_fForwardSpeed = 0.0f;
			m_fSideSpeed = 0.0f;
			m_fUpSpeed = 0.0f;
			wantToShoot(false);
			m_eEmpiresMortarActionState = EmpiresMortarActionState::COMPLETE;
			return;
		}

		if (now >= m_flEmpiresMortarActionShellDeadline)
			failEmpiresMortarAction("no-shell-created");
	}
}
