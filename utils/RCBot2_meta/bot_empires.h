#ifndef _BOT_EMPIRES_H_
#define _BOT_EMPIRES_H_

#include "bot.h"
#include "bot_empires_capture_retry.h"
#include "bot_empires_commander_economy.h"
#include "bot_empires_combat_relevance.h"
#include "bot_empires_combat_telemetry.h"
#include "bot_empires_start_anchor_recovery.h"
#include "bot_empires_commander_order_adapter.h"
#include "bot_empires_leader_actuation.h"
#include "bot_empires_vacancy_revive.h"
#include "bot_navigator.h"
#include "bot_navmesh_api.h"
#include "bot_squad_order_recipient.h"

#include <string>
#include <utility>
#include <vector>

constexpr int EMP_TEAM_NF = 2;
constexpr int EMP_TEAM_BE = 3;

constexpr int EMP_CLASS_SCOUT = 0;
constexpr int EMP_CLASS_RIFLEMAN = 1;
constexpr int EMP_CLASS_GRENADIER = 2;
constexpr int EMP_CLASS_ENGINEER = 3;

struct EmpiresBootstrapPlayerState
{
	int index = -1;
	int userId = -1;
	const edict_t* edict = nullptr;
	int team = -1;
	bool connected = false;
	bool active = false;
	bool fakeClient = false;
	bool observer = false;
	bool dead = true;
	bool alive = false;
	bool healthKnown = false;
	int health = -1;
	bool originKnown = false;
	Vector origin;
	bool velocityKnown = false;
	Vector velocity;
	bool lifeStateKnown = false;
	int lifeState = -1;
	bool classKnown = false;
	int classId = -1;
	bool classDesiredKnown = false;
	bool classDesired = false;
	bool appliedSidearmKnown = false;
	int appliedSidearm = -1;
	bool sidearmDesiredKnown = false;
	bool sidearmDesired = false;
	bool appliedPrimaryKnown = false;
	int appliedPrimary = -1;
	bool primaryDesiredKnown = false;
	bool primaryDesired = false;
	bool appliedGrenadeKnown = false;
	int appliedGrenade = -1;
	bool grenadeDesiredKnown = false;
	bool grenadeDesired = false;
	bool appliedEquipmentKnown = false;
	int appliedEquipment = -1;
	bool equipmentDesiredKnown = false;
	bool equipmentDesired = false;
	bool desiredSpawnKnown = false;
	int desiredSpawn = -1;
	bool respawnTimerKnown = false;
	float respawnTimer = 0.0f;
	bool currentTimeKnown = false;
	float currentTime = 0.0f;
	bool lastUserCommandKnown = false;
	int lastUserCommandNumber = 0;
	int lastUserCommandTick = 0;
	int lastUserCommandButtons = 0;
	float lastUserCommandForwardMove = 0.0f;
	float lastUserCommandSideMove = 0.0f;
	float lastUserCommandUpMove = 0.0f;
	int lastUserCommandImpulse = 0;
	int lastUserCommandWeaponSelect = 0;
};

enum class EmpiresBootstrapSpawnConfirmationState
{
	IDLE,
	ATTACK_PRESS_PENDING,
	ATTACK_RELEASE_PENDING,
	OBSERVING_RESULT,
	OBSERVING_TRAJECTORY,
	COMPLETE,
	FAILED
};

// Normal Empires bots use the same public spawn-selection path as a real
// client. The final confirmation is kept as a separate, bounded state so the
// generic bot AI cannot send movement or combat input before the game reports
// LIFE_RESPAWNABLE.
enum class EmpiresNormalSpawnState
{
	IDLE,
	SPAWNPOINT_REQUESTED,
	WAITING_FOR_RESPAWNABLE,
	ATTACK_FRAME_1,
	ATTACK_FRAME_2,
	RELEASE_FRAME,
	OBSERVING_RESULT,
	COMPLETE,
	FAILED
};

// A bounded route test for a normal Empires CBot. Unlike the controlled
// controller probes, this state uses the regular CBot::think() and
// CBot::runPlayerMove() path after the normal Empires spawn lifecycle.
enum class EmpiresNormalPathState
{
	IDLE,
	SEARCHING,
	ARMED,
	ACTIVE,
	COMPLETE,
	FAILED
};

// Development-only proof of the normal Empires vehicle-factory interaction
// path. The bot routes into a constructed team VF, opens the ordinary vehicle
// purchase flow, buys a vehicle through the public client-command path, then
// enters, drives, stops and exits through normal UserCmd input. No vehicle
// state or private game method is called by this action.
enum class EmpiresVehicleActionState
{
	IDLE,
	APPROACHING_TUTORIAL_EXIT,
	WAITING_FOR_TUTORIAL_TELEPORT,
	APPROACHING_FACTORY,
	OPENING_FACTORY,
	CONFIGURING_PURCHASE,
	WAITING_FOR_PURCHASE,
	APPROACHING_VEHICLE,
	WAITING_FOR_ENTRY,
	DRIVING,
	WAITING_FOR_EXIT,
	COMPLETE,
	FAILED
};

// Development-only, bounded proof for one natural NF mortar shot. This state
// never participates in production weapon selection or scoring.
enum class EmpiresMortarProbeState
{
	IDLE,
	WAITING_FOR_ACTIVE,
	WAITING_FOR_READY,
	COUCH_PREP,
	RELOAD_WAIT,
	ATTACK_ARMED,
	ATTACK_SENT,
	OBSERVING_SHELL,
	COMPLETE,
	FAILED
};

// A narrowly requested normal-path mortar action.  This is not tactical
// selection: it only exposes the already verified first-shot protocol after
// an explicit development command requests it.
enum class EmpiresMortarActionState
{
	IDLE,
	WAITING_FOR_ACTIVE,
	WAITING_FOR_READY,
	ATTACK_SUBMITTED,
	OBSERVING_TRAJECTORY,
	OBSERVING_RESULT,
	COMPLETE,
	FAILED
};

// Development-only proof that the current Empires engineer kit can repair a
// friendly building through the normal weapon/input path.  This is an
// explicit one-shot action, not Engineer role AI or generic weapon policy.
enum class EmpiresEngineerRepairActionState
{
	IDLE,
	WAITING_FOR_ACTIVE,
	REPAIRING,
	COMPLETE,
	FAILED
};

// Development-only proof of the current Engineer-kit build-item selection
// path. This sends one normal secondary-input pulse and observes the dynamic
// selection state; it does not place a building or participate in role AI.
enum class EmpiresEngineerBuildSelectionState
{
	IDLE,
	WAITING_FOR_ACTIVE,
	OBSERVING,
	COMPLETE,
	FAILED
};

// Development-only proof of the current Engineer-kit construction path. This
// owns one bounded selection/aim/primary-input window and never participates
// in normal Engineer role selection or build policy.
enum class EmpiresEngineerBuildActionState
{
	IDLE,
	WAITING_FOR_ACTIVE,
	WAITING_FOR_SELECTION,
	ATTACK_SUBMITTED,
	OBSERVING_RESULT,
	COMPLETE,
	FAILED
};

// Development-only proof of the current Empires EngineerCrate interaction.
// The action uses normal weapon selection and IN_USE input; it does not write
// ammo, player state or crate state and is not an autonomous resupply policy.
enum class EmpiresEngineerResupplyActionState
{
	IDLE,
	WAITING_FOR_ACTIVE,
	USING_SOURCE,
	OBSERVING_RESULT,
	COMPLETE,
	FAILED
};

// Development-only proof for one normal Empires infantry-mine throw.  Mine
// recognition remains outside generic weapon scoring; this state is entered
// only by an explicit bounded test command.
enum class EmpiresMineActionState
{
	IDLE,
	WAITING_FOR_ACTIVE,
	WAITING_FOR_READY,
	ATTACK_SUBMITTED,
	OBSERVING_RESULT,
	OBSERVING_DEPLOY,
	OBSERVING_TRIGGER,
	COMPLETE,
	FAILED
};

// Development-only proof of the current Empires binocular primary-attack
// protocol.  This owns one bounded select/aim/attack window only; Scout role
// policy and the already verified quick-spot bridge remain separate.
enum class EmpiresScoutBinocularActionState
{
	IDLE,
	WAITING_FOR_ACTIVE,
	AIMING,
	ATTACK_SUBMITTED,
	OBSERVING_RESULT,
	COMPLETE,
	FAILED
};

enum class EmpiresControllerProbeLifecycle
{
	INACTIVE,
	ENGINE_BOT_CREATED,
	SLOT_BOUND,
	CONTROLLER_VERIFIED,
	REMOVAL_PENDING
};

enum class EmpiresControllerProbeSpawnSequenceState
{
	IDLE,
	JOIN_NF_REQUESTED,
	OBSERVE_TEAM,
	RIFLEMAN_REQUESTED,
	OBSERVE_CLASS,
	SPAWNPOINT_REQUESTED,
	OBSERVE_SPAWNPOINT,
	ATTACK_PRESS_PENDING,
	ATTACK_RELEASE_PENDING,
	OBSERVE_SPAWN,
	COMPLETE,
	FAILED
};

enum class EmpiresControllerProbeSpawnHoldState
{
	IDLE,
	JOIN_NF_REQUESTED,
	OBSERVE_TEAM,
	RIFLEMAN_REQUESTED,
	OBSERVE_CLASS,
	SPAWNPOINT_REQUESTED,
	OBSERVE_SPAWNPOINT,
	WAITING_FOR_READY,
	ATTACK_FRAME_1,
	ATTACK_FRAME_2,
	RELEASE_FRAME,
	OBSERVE_RESULT,
	COMPLETE,
	FAILED
};

enum class EmpiresControllerProbeNeutralReadinessState
{
	IDLE,
	JOIN_NF_REQUESTED,
	OBSERVE_TEAM,
	RIFLEMAN_REQUESTED,
	OBSERVE_CLASS,
	SPAWNPOINT_REQUESTED,
	OBSERVE_SPAWNPOINT,
	NEUTRAL_SIMULATION,
	LIFE_RESPAWNABLE_REACHED,
	COMPLETE,
	FAILED
};

enum class EmpiresControllerProbeLoadoutReadinessState
{
	IDLE,
	JOIN_NF_REQUESTED,
	OBSERVE_TEAM,
	SET_SIDEARM,
	OBSERVE_SIDEARM,
	SET_PRIMARY,
	OBSERVE_PRIMARY,
	SET_GRENADE,
	OBSERVE_GRENADE,
	SET_EQUIPMENT,
	OBSERVE_EQUIPMENT,
	REQUEST_RIFLEMAN,
	OBSERVE_CLASS,
	SELECT_SPAWNPOINT,
	OBSERVE_SPAWNPOINT,
	NEUTRAL_SIMULATION,
	LIFE_RESPAWNABLE_REACHED,
	COMPLETE,
	FAILED
};

enum class EmpiresControllerProbeAppliedLoadoutReadinessState
{
	IDLE,
	JOIN_NF,
	OBSERVE_TEAM,
	REQUEST_RIFLEMAN,
	OBSERVE_CLASS_REQUEST,
	SET_SIDEARM,
	OBSERVE_SIDEARM,
	SET_PRIMARY,
	OBSERVE_PRIMARY,
	SET_GRENADE,
	OBSERVE_GRENADE,
	SET_EQUIPMENT,
	OBSERVE_APPLIED_LOADOUT,
	SELECT_SPAWN,
	OBSERVE_SPAWN,
	NEUTRAL_SIMULATION,
	LIFE_RESPAWNABLE_REACHED,
	COMPLETE,
	FAILED
};

// A separate one-press spawn-confirmation test. This state machine is entered
// only after applied-loadout readiness has completed and owns the only two
// UserCmd submissions in this test: one attack press and the next-frame
// neutral release.
enum class EmpiresControllerProbeSpawnConfirmReadyState
{
	IDLE,
	ATTACK_PRESS_PENDING,
	ATTACK_RELEASE_PENDING,
	OBSERVE_SPAWN,
	COMPLETE,
	FAILED
};

// One-shot movement proof entered only after spawn confirmation has completed.
// It owns a short forward input, one neutral release and bounded post-stop
// observations; normal bot AI remains suppressed throughout.
enum class EmpiresControllerProbeMovementState
{
	IDLE,
	MOVE_START_PENDING,
	MOVE_ACTIVE,
	MOVE_STOP_PENDING,
	FINAL_OBSERVE,
	COMPLETE,
	FAILED
};

// One-shot development proof that reuses the loaded waypoint data but does
// not enter the normal navigator, schedules or bot AI.  It selects one nearby
// waypoint and submits only direct forward input toward it.
enum class EmpiresControllerProbeWaypointState
{
	IDLE,
	TARGET_SELECTED,
	MOVE_ACTIVE,
	MOVE_STOP_PENDING,
	FINAL_OBSERVE,
	COMPLETE,
	FAILED
};

bool parseEmpiresBootstrapClass(const char* requested, int& classId, const char*& canonicalName);

// Production vehicle use (bot_empires_vehicle.cpp): buy at the team's vehicle
// factory, enter, drive with edge avoidance, fight, run over infantry.
// reverse-engineering/notes/vehicle-driving.md has the game facts.
enum class EmpiresVehicleStage
{
	NONE, GOTO_FACTORY, BUYING, WAIT_SPAWN, APPROACH, ENTERING, DRIVING,
	STUN_EXIT, STUN_THROW, GIVE_UP_EXIT, REPAIR_ASSIST
};

int EmpiresVehicleDebugScaleHealth(float factor, const float* sideFactors);

// Called on map change: expiry times count in curtime and entries are keyed by entity index.
void EmpiresTurretBuildLevelReset();
void EmpiresReviveLevelReset();
void EmpiresVehicleLevelReset();
void EmpiresCommanderEconomyLevelReset();
void EmpiresObjectiveLevelReset();

struct EmpiresVehicleBrain
{
	EmpiresVehicleStage stage = EmpiresVehicleStage::NONE;
	MyEHandle vehicle;
	MyEHandle factory;
	MyEHandle target;
	int vehicleIndex = -1;
	int factoryIndex = -1;
	bool buyInPlace = false;
	bool wantJeep = false;
	int purchaseStep = 0;
	float factoryProbeAt = 0.0f; // diagnostic: log the factory state once in WAIT_SPAWN
	std::vector<Vector> factoryRoute; // corners around the factory to its ramp
	std::size_t factoryRouteIndex = 0;
	float nextAction = 0.0f;
	float stageUntil = 0.0f;
	float cooldownUntil = 0.0f;
	float nextDecision = 0.0f;
	int failures = 0;
	int useTries = 0;
	std::vector<int> baseline;
	// Driving state.
	Vector lastVehicleOrigin = Vector(0, 0, 0);
	float lastSampleTime = 0.0f;
	Vector velocity = Vector(0, 0, 0);
	float slowSince = 0.0f;
	float reverseUntil = 0.0f;
	float reverseSide = 0.0f;
	int stuckReverses = 0;
	float movingSince = 0.0f;
	int abandonedVehicle = -1;
	float abandonedUntil = 0.0f;
	float nextTargetPick = 0.0f;
	float aimSince = 0.0f;
	float nextLog = 0.0f;
	Vector goal = Vector(0, 0, 0);
	bool goalValid = false;
	// Navmesh route to the drive goal (vehicle path search).
	std::vector<Vector> route;
	std::vector<unsigned int> routeAreas;
	std::size_t routeIndex = 0;
	Vector routeGoal = Vector(0, 0, 0);
	float nextReplan = 0.0f;
	bool routeFailed = false;
	bool routePartial = false; // route ends at the reachable area nearest the goal
	int routeResult = -1;
	float width = 0.0f;
	float bailLogAt = 0.0f; // diagnosis: log the bot's position after a bail-out (0.3 s, 1 s, 3 s)
	int bailLogStep = 0;
	float stageSince = 0.0f; // curtime of the last stage change (bail-out waits 1 s after boarding)
	// Damage: retreat to a repair station or the base and call engineers.
	float maxHealth = 0.0f;
	int armorMaxSeen[4] = {0, 0, 0, 0};
	int worstSide = -1;
	float nextFaceLog = 0.0f;
	// Engineer: side of the vehicle being worked on and when it last improved.
	int assistSide = -1;
	float assistLastTotal = -1.0f;
	float assistLastRise = 0.0f;
	Vector assistWalkFrom = Vector(0, 0, 0);
	float assistWalkSince = 0.0f;
	bool assistOverride = false;
	float lastHealth = 0.0f;
	bool retreating = false;
	bool parkValid = false;
	bool homeParkValid = false;
	// Commander drives the CV from the driver seat (still commander): to a goal
	// set by the commander economy; no retreat, no weapons.
	bool commanderCV = false;
	Vector cvGoal = Vector(0, 0, 0);
	float cvArrive = 300.0f;
	bool cvArrived = false;
	const char* cvEnd = nullptr;
	// Boomtanking: run over enemy infantry inside the base zone, then back to cvGoal.
	bool cvHunt = false;
	Vector cvHome = Vector(0, 0, 0);
	float cvZone = 0.0f;
	int cvKillsTried = 0;
	int cvLastTarget = -1;
	float cvNoPreySince = 0.0f;
	float cvBackUntil = 0.0f; // three-point turn: reversing with opposite lock until then
	float cvBackNext = 0.0f;  // no new three-point turn before then
	// Armed fight: keep moving at range, circling the target, direction changes now and then.
	float fightDir = 0.0f;     // +1 / -1 around the target, 0 = not chosen yet
	float fightSwitch = 0.0f;  // next change of direction
	float fightSight = 0.0f;   // last time the target was in sight
	bool fightMove = false;    // circling this frame
	// Target motion for leading shots.
	int aimTarget = -1;
	Vector aimLastPos = Vector(0, 0, 0);
	float aimLastTime = 0.0f;
	Vector aimVel = Vector(0, 0, 0);
	float nextAimLog = 0.0f;
	float nextFightLog = 0.0f;
	bool refitDone = false;
	int refitStep = 0;
	int padStation = -1;
	int parkCount = 0;
	int padEntryAttempt = 0;
	float parkSince = 0.0f;
	bool padInside = false;
	bool padEntryValid = false;
	Vector padEntry = Vector(0, 0, 0);
	float refitNext = 0.0f;
	Vector homePark = Vector(0, 0, 0);
	Vector homeParkFor = Vector(0, 0, 0);
	Vector park = Vector(0, 0, 0);
	float lastHealthRise = 0.0f;
	float nextRepairCall = 0.0f;
	float retreatCooldownUntil = 0.0f;
	// Engineer repairing a team vehicle (REPAIR_ASSIST).
	MyEHandle assistVehicle;
	float nextAssistNav = 0.0f;
	Vector assistNavGoal = Vector(0, 0, 0);
	// Final command for this frame.
	float forward = 0.0f;
	float side = 0.0f;
	int buttons = 0;
	QAngle view = QAngle(0, 0, 0);
	// Bill Nye: push enemy vehicles off the map, stun them when stuck.
	bool billNye = false;
	bool ramming = false;
	int ramTargetIndex = -1; // vehicle Bill Nye was last ramming
	Vector pushDir = Vector(0, 0, 0);
	float contactSince = 0.0f;
	Vector targetStart = Vector(0, 0, 0);
	float stunCooldownUntil = 0.0f;
	float throwPressUntil = 0.0f;
	bool thrown = false;
};

// Minimal Empires bot type used by the controlled native bootstrap test.
// Game-specific behavior is intentionally deferred until the ABI and bot
// lifecycle are tested with a controlled fake client.
class CBotEmpires : public CBot
{
public:
	CBotEmpires();
	~CBotEmpires() override;

	bool attachBootstrapEdict(edict_t* pEdict);
	bool attachControllerProbeEdict(edict_t* pEdict, int slot);
	bool requestBootstrapRemoval();
	bool requestControllerProbeRemoval();
	bool removalRequested() const { return m_bRemovalRequested; }
	bool isBootstrapOnly() const override { return m_bBootstrapOnly; }
	bool isControllerProbeOnly() const { return m_bControllerProbeOnly; }
	bool isControlledTestOnly() const override { return m_bBootstrapOnly || m_bControllerProbeOnly; }
	int getTeam() override;
	bool isEnemy(edict_t* pEdict, bool bCheckWeapons = true) override;
	float getEnemyFactor(edict_t* pEnemy) override;
	void enemyFound(edict_t* pEnemy) override;
	void receiveEmpiresSpottedEnemy(edict_t* pEnemy, int sourceIndex);
	void updateConditions() override;
	bool canGotoWaypoint(const Vector& vPrevWaypoint, CWaypoint* pWaypoint,
		CWaypoint* pPrev = nullptr) override;
	EmpiresControllerProbeLifecycle controllerProbeLifecycle() const { return m_eControllerProbeLifecycle; }
	const char* controllerProbeLifecycleName() const;
	int controllerProbeSlot() const { return m_iControllerProbeSlot; }
	int controllerProbeClientIndex() const { return m_iControllerProbeClientIndex; }
	EmpiresControllerProbeSpawnSequenceState controllerProbeSpawnSequenceState() const
	{
		return m_eControllerProbeSpawnSequenceState;
	}
	const char* controllerProbeSpawnSequenceStateName() const;
	const char* controllerProbeSpawnHoldStateName() const;
	edict_t* getCurrentControlledTestEdict() const;
	bool requestControllerProbeTeam(int requestedTeam);
	bool requestControllerProbeSpawnSequence();
	bool requestControllerProbeSpawnHoldTest();
	bool requestControllerProbeReadinessTest();
	bool requestControllerProbeNeutralReadiness();
	bool requestControllerProbeLoadoutReadiness();
	bool requestControllerProbeLoadoutReadinessClassFirst();
	bool requestControllerProbeAppliedLoadoutReadiness();
	bool requestControllerProbeSpawnConfirmReady();
	const char* controllerProbeSpawnConfirmReadyStateName() const;
	bool requestControllerProbeMovementProof();
	const char* controllerProbeMovementStateName() const;
	bool requestControllerProbeWaypointOnce();
	bool requestControllerProbeWaypointRouteOnce();
	bool requestControllerProbePathOnce(int startWaypoint = -1,
		int destinationWaypoint = -1);
	const char* controllerProbeWaypointStateName() const;
	void setup() override;
	bool suppressBotThink() override;
	bool startGame() override;
	bool hurt(edict_t* pAttacker, int iHealthNow, bool bDontHide = false) override;
	void died(edict_t* pKiller, const char* pszWeapon) override;
	void killed(edict_t* pVictim, char* weapon) override;
	void modThink() override;
	void getTasks(unsigned iIgnore = 0) override;
	void handleWeapons() override;
	void freeMapMemory() override;
	bool requestNormalPathOnce(int startWaypoint = -1,
		int destinationWaypoint = -1, bool terminalAfterStop = false);
	bool requestEmpiresVehicleFactoryDriveOnce();
	// Vehicle use (bot_empires_vehicle.cpp).
	void tickEmpiresVehicle();
	void tickEmpiresVoidSuicide();
	float m_flVoidNextCheck = 0.0f;
	float m_flVoidSince = -1.0f;
	bool m_bVoidKilled = false;
	bool empiresVehicleOwnsBot() const
	{
		return m_vehicle.stage != EmpiresVehicleStage::NONE;
	}
	bool applyEmpiresVehicleCommand(CBotCmd& command);
	void resetEmpiresVehicle(const char* reason, float cooldown);
	bool isEmpiresBillNye() const;
	// Engineer walks to a team vehicle that called for repairs (getTasks).
	bool handleEmpiresVehicleRepairRequest();
	EmpiresVehicleBrain m_vehicle;
	// Engineer skills (changeskill <slot> <code>, CEmpPlayer::ChangeSkill).
	void updateEmpiresEngineerSkills();
	int m_iEmpiresSkillsSent[4] = {0, 0, 0, 0};
	float m_flEmpiresSkillsCheck = 0.0f;
	int m_iEmpiresSkillFlags = -1;
	int m_iEmpiresSkillRank = -1;
	friend int EmpiresVehicleReadInt(const CBotEmpires*, const edict_t*, const char*, int);
	friend int EmpiresVehiclePlayerVehicle(const CBotEmpires*, const edict_t*);
	friend bool EmpiresVehicleAlive(const CBotEmpires*, const edict_t*);
	// Walks to the team's Command Vehicle, enters it and takes the Commander
	// seat. Stage 4 means the bot is seated and stays there.
	bool requestCommanderVehicleEntryOnce();
	// The seated commander, moved to the CV driver seat, drives it to goal (vehicle brain).
	bool startEmpiresCommanderDrive(edict_t* cv, const Vector& goal);
	void handleCommanderVehicleEntry();
	bool commanderVehicleEntryActive() const { return m_iCommanderEntryStage > 0; }
	int m_iCommanderEntryStage = 0;
	int m_iCommanderEntryUserId = -1;
	MyEHandle m_hCommanderEntryVehicle;
	float m_fCommanderEntryDeadline = 0.0f;
	Vector m_vCommanderEntrySpot = Vector(0.0f, 0.0f, 0.0f);
	float m_fCommanderEntryNextReplan = 0.0f;
	float m_fCommanderEntryUseAt = 0.0f;      // when IN_USE was tapped (stage 2)
	bool m_bCommanderEntryFarUseTried = false; // one USE from > 200 per approach
	// Commander Engineer out of the CV to finish a building himself: 1 leaving the seat,
	// 2 building (build-assign), then the seat choice puts him back in.
	int m_iCommanderBuildStage = 0;
	MyEHandle m_hCommanderBuildTarget;
	float m_fCommanderBuildDeadline = 0.0f;
	Vector m_vCommanderBuildDriveGoal = Vector(0.0f, 0.0f, 0.0f); // stages 3 driver seat, 4 driving
	bool m_bCommanderBuildTurret = false; // target is a refinery: stage 5 = refinery-turret job
	bool m_bCommanderTurretStarted = false;
	bool m_bCommanderHunt = false; // stages 3/4 for boomtanking instead of a building
	float m_fCommanderHuntZone = 0.0f;
	// Automatic Commander (bot_empires_commander_economy.cpp, cvar rcbot_empires_auto_commander).
	void tickCommanderEconomy();
	// Test-only: act as if a human of the team wanted the Commander seat.
	static void requestCommanderYieldTest(int team);
	bool isEmpiresEngineerClass() const;
	struct CommanderEconomyState
	{
		float nextTick = 0.0f;
		float nextEntryTry = 0.0f;
		int entryAttempts = 0;
		int lastSelfBuildSkip = 0;
		float nextTurretCheck = 0.0f;
		float nextHuntCheck = 0.0f;
		float blockedUntil[EmpiresCommanderEconomy::TypeCount] = {};
		int failures[EmpiresCommanderEconomy::TypeCount] = {};
		bool verifyPending = false;
		int verifyType = 0;
		int verifyResIndex = -1;
		float verifyAt = 0.0f;
		float verifyResources = 0.0f;
		std::vector<int> verifyBefore;
		std::vector<std::pair<int, float>> refineryPointBlocked; // res index, until
		struct AssignTry { int entity = 0; int tries = 0; float next = 0.0f; };
		std::vector<AssignTry> assignTries;
		int lastLogKey = -1;
		float escortUntil = 0.0f; // an Engineer walks toward a resource point out of reach
		float nextResearch = 0.0f;
		int lastResearchActive = -1;
		bool lastResearchPaused = false;
	} m_commanderEconomy;
	// An Engineer walks to one assigned building and works on it until full.
	bool requestEmpiresBuildAssignment(edict_t* building);
	void tickEmpiresBuildAssignment();
	void tickEmpiresGuardOrder();
	float m_fEmpiresGuardNextCheck = 0.0f;
	int m_iEmpiresGuardLoggedTarget = -1;
	int m_iEmpiresTestGuardTarget = 0; // test-only: entindex acting as Guard target
	void queueEmpiresBuildAssignRoute();
	int m_iBuildAssignStage = 0;
	int m_iBuildAssignAttempts = 0;
	MyEHandle m_hBuildAssignBuilding;
	float m_fBuildAssignStart = 0.0f;
	float m_fBuildAssignNextTry = 0.0f;
	Vector m_vBuildAssignSpot = Vector(0.0f, 0.0f, 0.0f);
	bool findEmpiresKitAim(edict_t* building, const Vector& eye, Vector& aim, bool keepAim = false) const;
	float m_fBuildAssignNextLog = 0.0f;
	Vector m_vEmpiresKitAim = Vector(0.0f, 0.0f, 0.0f);
	bool findEmpiresBuildStandSpot(edict_t* building, Vector& spot) const;
	// Test-only input bridge for the same consumer used by the public
	// Empires Commander MOVE SendProps.  It does not teleport or write game
	// Commander state.
	bool requestEmpiresCommanderMoveTest(const Vector& target);
	void damageEmpiresTestGuardBuilding(int entIndex);
	void setEmpiresTestGuardTarget(int entIndex) { m_iEmpiresTestGuardTarget = entIndex; m_fEmpiresGuardNextCheck = 0.0f; }
	// Test-only Phase-10.6 path: issue a SQUAD parent from the authoritative
	// live bot-owned membership snapshot.  It never changes game membership or
	// starts navigation.
	bool requestEmpiresSquadMoveTestFixture(int team, int squadId,
		const Vector& target);
	// Test-only Recovery Episode acceptance fixture.  The capture phases use
	// the normal selector and the real domain-exhaustion/progress hooks; they
	// do not expose or mutate production policy state directly.
	bool requestEmpiresRecoveryCaptureAcceptance(const char* phase);
	// Test-only SQUAD recovery acceptance fixture.  It drives the existing
	// bounded shared-goal recovery branch and leaves the shared goal active.
	bool requestEmpiresRecoverySquadAcceptance();
	bool requestEmpiresSquadSpatialNavigation(
		const RCBotSquadOrder::NavigationIntentRequest& request,
		std::uint64_t* requestToken);
	void cancelEmpiresSquadSpatialNavigation(
		const RCBotSquadOrder::NavigationIntentRequest& request,
		std::uint64_t requestToken);
	bool requestEmpiresMortarSmoke();
	bool requestEmpiresMortarFireOnce();
	bool requestEmpiresMortarAtEnemyOnce(edict_t* pTarget);
	bool requestEmpiresMortarAtPointOnce(const Vector& target, bool checkArc = false);
	bool requestEmpiresMortarAimOnce(int targetWaypoint = 149);
	bool prepareEmpiresEngineerRepairFixture(int buildingIndex = -1);
	bool damageEmpiresEngineerRepairFixture(int damage = 100,
		const char* buildingClass = "emp_building_nf_barracks");
	bool scanEmpiresEngineerRepairVisibility(int buildingIndex = -1);
	bool scanEmpiresEngineerRepairVisibilityAllBuildings();
	bool requestEmpiresEngineerRepairOnce(int buildingIndex = -1,
		bool requireFullHealth = false);
	bool requestEmpiresEngineerBuildSelectionOnce();
	bool requestEmpiresEngineerBuildOnce(int targetWaypoint = 147,
		const Vector* targetOverride = nullptr,
		bool requireFullHealth = false);
	bool requestEmpiresEngineerResupplyOnce(bool fixtureMode = true,
		bool depletionOnly = false);
	bool requestEmpiresMineFireOnce();
	bool requestEmpiresMineTriggerOnce(int targetUserId,
		float triggerDelaySeconds = 0.0f);
	static bool requestEmpiresScoutBinocularOnce(int scoutUserId,
		int targetUserId);
	static bool requestEmpiresScoutBinocularBuildingOnce(int scoutUserId,
		int buildingIndex);
	static bool observeEmpiresScoutSkillsOnce(int scoutUserId);
	static bool observeEmpiresSquadProbeOnce(int targetUserId);
	static bool requestEmpiresSquadLeadActuation(int issuerUserId,
		int targetUserId, int contextGeneration = 0);
	static RCBotEmpiresLeadActuation::LeadActuationStatus
		pollEmpiresSquadLeadActuation();
	const char* normalPathStateName() const;
	bool normalPathRouteMatches(int startWaypoint, int destinationWaypoint) const;
	bool normalPathRouteMatches(const std::vector<int>& expectedRoute) const;
	bool hasEmpiresFixtureEnemy(const edict_t* pTarget);
	static bool requestEmpiresEnemyLosFixture(int observerUserId, int opponentUserId,
		bool requireNaturalExpiry = false, bool combatMode = false,
		bool releaseObserverForCombatMovement = false);
	static void clearEmpiresEnemyLosFixture(const char* reason);
	static bool requestEmpiresScoutPriorityFixture(int scoutUserId,
		int targetAUserId, int targetBUserId, int firingTargetUserId);
	static void clearEmpiresScoutPriorityFixture(const char* reason);
	void alterOutgoingUserCmd(CBotCmd& command) override;

	bool readBootstrapPlayerState(EmpiresBootstrapPlayerState& state) const;
	bool requestBootstrapTeam(int requestedTeam);
	bool requestBootstrapClass(int requestedClass);
	bool requestBootstrapSpawn();
	bool requestBootstrapSpawnConfirmation();

private:
	int classifyEmpiresPlayer(edict_t* pEdict) const;
	static bool isEmpiresRepairableBuildingClass(const char* className,
		int team);
	void observeEmpiresPlayerConditions();
	void observeEmpiresSquadRuntimeShadow();
	void observeEmpiresSquadGameplay();
	bool handleEmpiresSquadGoal();
	// Goal-Roam replacement for maps without waypoints (navmesh only).
	bool handleEmpiresNavmeshGoal();
	// Hostile MG turret: note it when it hurts us, back out of its range, keep goals out of it.
	void noteEmpiresTurretThreat(edict_t* pAttacker, const Vector* damageOrigin = nullptr);
	bool handleEmpiresTurretThreat();
	// Engineer with the Revive skill revives dead teammates (bot_empires_revive.cpp).
	bool handleEmpiresRevive();
	int m_iReviveTarget = 0;
	float m_flReviveUntil = 0.0f;
	float m_flReviveNextNav = 0.0f;
	float m_flReviveSent = 0.0f;
	float m_flReviveNextSearch = 0.0f;
	float m_flReviveNextLog = 0.0f;
	float m_flReviveDeathTime = 0.0f;
	bool m_bReviveHoldLogged = false;
	bool m_bReviveHoldActive = false;
	bool holdRespawnForRevive();
	// Engineer builds an MG turret on the front side of own refineries (bot_empires_turret_build.cpp).
	bool handleEmpiresRefineryTurret();
	int m_iTurretRefinery = 0;
	int m_iTurretAttempt = 0;
	int m_iTurretBuilding = 0;
	int m_iTurretAssignRejects = 0;
	bool m_bEmpiresRepairRole = false; // skill plan: Repair first (builds turrets); Revive role stays with the team
	bool m_bTurretBuildArmed = false;
	Vector m_vTurretSpot;
	float m_flTurretYaw = 0.0f;
	float m_flTurretUntil = 0.0f;
	float m_flTurretNextNav = 0.0f;
	float m_flTurretNextSearch = 0.0f;
	float m_flTurretNextLog = 0.0f;
	bool handleEmpiresGrenadierTurretAttack();
	bool handleEmpiresDoorButton();
	bool handleEmpiresTurretRocketFallback(edict_t* turret, float range, int health, bool logFailure);
	void tickEmpiresTurretRocket();
	bool isInsideEmpiresTurretThreat(const Vector& point);
	bool isEmpiresTurretThreatOnLine(const Vector& a, const Vector& b);
	bool pickEmpiresNavmeshGoal();
	void handleEmpiresDefendMines(float now);
	int countOwnEmpiresMines(std::vector<Vector>* positions = nullptr);
	void endEmpiresNavmeshGoal(const char* event, const char* reason,
		float avoidSeconds);
	int findEmpiresSquadLeader(int team, int squad) const;
	void updateEmpiresVacancyReviveIntervention();
	static bool readLeadActuationParty(CBotEmpires* reader,
		RCBotEmpiresLeadActuation::LeadActuationParty& party);
	int countEmpiresSquadBots(int team, int squad) const;
	void resetEmpiresSquadGoalState();
	void ensureEmpiresRecoveryEpisode(RCBotRecovery::Domain domain,
		const char* subjectId, std::uint64_t domainGeneration,
		const char* reason);
	void reuseEmpiresRecoveryEpisode(const char* reason);
	void endEmpiresRecoveryEpisode(const char* reason);
	void observeEmpiresRecoveryEpisode();
	void observeEmpiresRecoveryDomainExhausted(const char* reason);
	void observeEmpiresRecoveryProgress(const char* reason);
	bool isEmpiresCaptureCandidateSuppressed(int pointIndex);
	bool hasActiveEmpiresSquadRecoveryGoal() const;
	int recoveryEpisodeBotId() const;
	void maybeSpotEmpiresEnemy();
	void observeNormalSpawn();
	void logNormalSpawnInventory() const;
	bool submitNormalSpawnUserCmd(int buttons, const char* phase);
	void observePendingTeamChange();
	void observePendingClassChange();
	void observePendingSpawn();
	void processBootstrapSpawnConfirmation();
	void observeControllerProbeSpawnSequence();
	void observeControllerProbeSpawnHoldTest();
	void observeControllerProbeNeutralReadiness();
	void observeControllerProbeLoadoutReadiness();
	void observeControllerProbeAppliedLoadoutReadiness();
	void observeControllerProbeSpawnConfirmReady();
	void observeControllerProbeMovementProof();
	void observeControllerProbeWaypointOnce();
	void observeControllerProbePathOnce();
	void continueNormalPathSearch(bool restart);
	bool readEmpiresCommanderMove(Vector& target, bool& synthetic,
		bool& expired, int& commandType) const;
	void onNavigationIntentStarted(
		const RCBotNavigation::NavigationIntent& intent) override;
	void onNavigationAttemptStarted(
		const RCBotNavigation::NavigationIntent& intent) override;
	void onNavigationIntentTerminal(
		const RCBotNavigation::NavigationIntent& intent,
		RCBotNavigation::Result result, const char* reason) override;
	bool isEmpiresCommanderMoveActive() const;
	bool handleEmpiresCommanderMoveProviderNeutral();
	bool ownsEmpiresSquadSpatialMovement() const;
	void logEmpiresCommanderMoveDiagnostic(const char* phase,
		const CBotCmd* outgoing = nullptr, bool force = false);
	void clearEmpiresCommanderMoveState(const char* reason);
	bool requestEmpiresCaptureApproach();
	bool isEmpiresCaptureApproachPointStillValid();
	// Diagnostic-only stable identity for combat/objective event correlation.
	// The returned string contains values, never entity pointers.
	std::string getEmpiresCombatCorrelation(const edict_t* target = nullptr);
	// Diagnostic-only route-attempt identity. This updates no gameplay policy;
	// it is advanced only when an existing capture route is issued.
	void beginEmpiresCaptureRouteAttempt();
	// Diagnostic-only: emits CAPTURE_ROUTE_TERMINAL when a capture route
	// drains before arrival. Read-only; never changes retry/latch/nav state.
	void emitEmpiresCaptureRouteTerminal(const bool combatResume,
		const bool consumeRetry, const char* decision,
		const bool combatLatchBefore = false,
		const bool objectiveValidBefore = false,
		const int retryBefore = -1,
		const std::uint64_t nextAttemptGeneration = 0);
	// Clears start-anchor recovery state with the capture context it is
	// bound to. Called at every site that tears down capture state.
	void clearEmpiresStartAnchorRecovery();
	// Read-only anchor probe for the live capture route: reports the current
	// RouteAnchorStatus without starting movement. Never changes nav state.
	RouteAnchorStatus probeEmpiresCaptureRouteAnchors();
	// Starts one bounded re-anchor leg when a capture drain meets the
	// NO_START_ANCHOR zero-progress contract. Issues the schedule and
	// telemetry; consumes no retry budget. Returns true when issued.
	bool tryStartEmpiresStartAnchorRecovery(const bool combatResume);
	// Finishes a drained recovery leg: re-issues the normal capture route
	// when anchors are resolvable again, else records failure and returns
	// false so existing retry/reject accounting takes over.
	bool tryFinishEmpiresStartAnchorRecovery(const bool combatResume);
	// Telemetry-only: logs and records a START_ANCHOR_RECOVERY_* event with
	// userid, point, start/end position, displacement, duration, attempt,
	// and resulting anchor status. Never changes behavior state.
	void recordEmpiresStartAnchorRecoveryEvent(const char* eventName,
		const char* reason, const Vector& startOrigin, float startTime,
		int reentryWaypoint);
	void prepareEmpiresEnemyLosFixture();
	bool beginNormalRespawn();
	void logControllerProbeSpawnSequenceState(const char* phase,
		const EmpiresBootstrapPlayerState& state) const;
	void logControllerProbeSpawnHoldState(const char* phase,
		const EmpiresBootstrapPlayerState& state) const;
	void recordControllerProbeReadinessChanges(
		const EmpiresBootstrapPlayerState& state);
	bool issueControllerProbeClientCommand(const char* command, const char* phase);
	bool submitControllerProbeUserCmd(int buttons, const char* phase);
	bool issueControllerProbeNeutralClientCommand(const char* command, const char* phase);
	bool submitControllerProbeNeutralUserCmd(const char* phase);
	bool controllerProbeSpawnSequenceIdentityMatches(
		const EmpiresBootstrapPlayerState& state) const;
	void failControllerProbeSpawnSequence(const char* reason);
	bool controllerProbeSpawnHoldIdentityMatches(
		const EmpiresBootstrapPlayerState& state) const;
	void failControllerProbeSpawnHoldTest(const char* reason);
	void failControllerProbeNeutralReadiness(const char* reason);
	bool controllerProbeReadinessTimedOut(
		const EmpiresBootstrapPlayerState& state) const;
	bool requestControllerProbeSpawnHoldTestInternal(bool readinessOnly);
	bool controllerProbeNeutralIdentityMatches(
		const EmpiresBootstrapPlayerState& state) const;
	const char* controllerProbeNeutralReadinessStateName() const;
	void logControllerProbeNeutralReadinessState(const char* phase,
		const EmpiresBootstrapPlayerState& state) const;
	void recordControllerProbeNeutralReadinessChanges(
		const EmpiresBootstrapPlayerState& state);
	void recordControllerProbeAppliedLoadoutReadinessChanges(
		const EmpiresBootstrapPlayerState& state);
	bool controllerProbeLoadoutIdentityMatches(
		const EmpiresBootstrapPlayerState& state) const;
	bool controllerProbeAppliedLoadoutIdentityMatches(
		const EmpiresBootstrapPlayerState& state) const;
	const char* controllerProbeLoadoutReadinessStateName() const;
	const char* controllerProbeAppliedLoadoutReadinessStateName() const;
	void logControllerProbeLoadoutReadinessState(const char* phase,
		const EmpiresBootstrapPlayerState& state) const;
	void logControllerProbeAppliedLoadoutReadinessState(const char* phase,
		const EmpiresBootstrapPlayerState& state) const;
	void recordControllerProbeLoadoutReadinessChanges(
		const EmpiresBootstrapPlayerState& state);
	void failControllerProbeLoadoutReadiness(const char* reason);
	void failControllerProbeAppliedLoadoutReadiness(const char* reason);
	void failControllerProbeSpawnConfirmReady(const char* reason);
	bool requestControllerProbeLoadoutReadinessInternal(bool classFirst);
	bool issueControllerProbeLoadoutClientCommand(const char* command,
		const char* phase);
	bool issueControllerProbeAppliedLoadoutClientCommand(const char* command,
		const char* phase);
	bool submitControllerProbeLoadoutNeutralUserCmd(const char* phase);
	bool submitControllerProbeAppliedLoadoutNeutralUserCmd(const char* phase);
	bool submitControllerProbeSpawnConfirmReadyUserCmd(int buttons, const char* phase);
	bool controllerProbeSpawnConfirmReadyIdentityMatches(
		const EmpiresBootstrapPlayerState& state) const;
	void logControllerProbeSpawnConfirmReadyState(const char* phase,
		const EmpiresBootstrapPlayerState& state) const;
	bool submitControllerProbeMovementUserCmd(int buttons, float forwardmove,
		const char* phase, const QAngle* viewAngles = nullptr);
	bool controllerProbeMovementIdentityMatches(
		const EmpiresBootstrapPlayerState& state) const;
	void logControllerProbeMovementState(const char* phase,
		const EmpiresBootstrapPlayerState& state) const;
	void failControllerProbeMovement(const char* reason);
	bool controllerProbeWaypointIdentityMatches(
		const EmpiresBootstrapPlayerState& state) const;
	void logControllerProbeWaypointState(const char* phase,
		const EmpiresBootstrapPlayerState& state) const;
	void failControllerProbeWaypoint(const char* reason);
	void failControllerProbePath(const char* result, const char* reason);
	bool submitBootstrapUserCmd(int buttons, const char* phase);
	void failBootstrapSpawnConfirmation(const char* reason);
	bool readBootstrapClass(int& classId, bool& classDesired, bool& classDesiredKnown,
		const char*& propertyName) const;
	bool readEmpiresClass(const edict_t* pEdict, int& classId, bool& classDesired,
		bool& classDesiredKnown, const char*& propertyName) const;
	bool resolveBootstrapSpawnId(int team, int& spawnId, edict_t*& spawnEdict) const;
	bool readBootstrapIntProperty(const char* propertyName, int& value) const;
	bool readBootstrapBoolProperty(const char* propertyName, bool& value) const;
	bool readBootstrapFloatProperty(const char* propertyName, float& value) const;
	bool readEmpiresIntProperty(const edict_t* pEdict, const char* propertyName, int& value) const;
	bool readEmpiresBoolProperty(const edict_t* pEdict, const char* propertyName, bool& value) const;
	bool readEmpiresFloatProperty(const edict_t* pEdict, const char* propertyName, float& value) const;
	bool readEmpiresWeaponBoolProperty(const edict_t* pEdict,
		const char* propertyName, bool& value) const;
	bool readEmpiresWeaponFloatProperty(const edict_t* pEdict,
		const char* propertyName, float& value) const;
	bool readEmpiresWeaponHandleProperty(const edict_t* pEdict,
		const char* propertyName, int& entityIndex) const;
	bool readEmpiresEntityHandleProperty(const edict_t* pEdict,
		const char* propertyName, int element, int& entityIndex) const;
	bool findEmpiresTutorialExit(int team, edict_t*& exitTrigger,
		Vector& origin) const;
	bool findEmpiresVehicleFactory(int team, edict_t*& factory,
		Vector& origin, int& factoryTeam) const;
	bool findEmpiresVehicle(int team, edict_t*& vehicle, Vector& origin,
		int& vehicleTeam, int& passenger0,
		const std::vector<int>* excludedIndices = nullptr) const;
	bool isEmpiresVehicleActionActive() const;
	void handleEmpiresVehicleAction();
	void failEmpiresVehicleAction(const char* reason);
	void observeEmpiresMortarSmoke();
	void failEmpiresMortarSmoke(const char* reason);
	bool isEmpiresMortarProbeActive() const;
	void handleEmpiresMortarAction();
	bool findEmpiresMortarAim(const Vector& eye, const Vector& target, bool checkArc,
		QAngle& angles, Vector& launchOrigin, Vector& hitPoint) const;
	void failEmpiresMortarAction(const char* reason);
	bool isEmpiresMortarActionActive() const;
	bool isEmpiresEngineerRepairActionActive() const;
	bool isEmpiresEngineerBuildSelectionActive() const;
	bool isEmpiresEngineerBuildActionActive() const;
	bool isEmpiresEngineerResupplyActionActive() const;
	bool isEmpiresEngineerResupplyNeeded() const;
	bool findEmpiresEngineerNearbySupportTarget(int& buildingIndex) const;
	bool findEmpiresEngineerSupportTarget(int& buildingIndex,
		int& waypointId, Vector& waypointOrigin) const;
	bool findEmpiresEngineerAmmoSource(int team, edict_t*& source,
		Vector& sourceOrigin) const;
	void interruptEmpiresEngineerSupportForCombat(const char* reason);
	void interruptEmpiresEngineerResupplyForCombat(const char* reason);
	void handleEmpiresEngineerRepairAction();
	void failEmpiresEngineerRepairAction(const char* reason);
	void handleEmpiresEngineerBuildSelection();
	void failEmpiresEngineerBuildSelection(const char* reason);
	void handleEmpiresEngineerBuildAction();
	void failEmpiresEngineerBuildAction(const char* reason);
	void handleEmpiresEngineerResupplyAction();
	void failEmpiresEngineerResupplyAction(const char* reason);
	bool isEmpiresMineActionActive() const;
	void handleEmpiresMineAction();
	void failEmpiresMineAction(const char* reason);
	bool isEmpiresScoutBinocularActionActive() const;
	void handleEmpiresScoutBinocularAction();
	void failEmpiresScoutBinocularAction(const char* reason);
	bool requestEmpiresScoutBinocularTarget(edict_t* pTargetEdict,
		bool targetBuilding = false);
	bool readEmpiresEntityIntProperty(const edict_t* pEdict,
		const char* propertyName, int& value) const;
	bool readEmpiresEntityBoolProperty(const edict_t* pEdict,
		const char* propertyName, bool& value) const;
	bool writeEmpiresEntityIntProperty(const edict_t* pEdict,
		const char* propertyName, int value) const;
	void debugBeforeRunPlayerMove(const CBotCmd& command) override;
	void debugAfterRunPlayerMove(const CBotCmd& command) override;
	void logEmpiresMortarRunPlayerMove(const char* boundary,
		const char* capture, const CBotCmd& command);
	bool readEmpiresDebugIntProperty(const edict_t* pEdict,
		const char* propertyName, int& value) const;
	bool readEmpiresDebugFloatProperty(const edict_t* pEdict,
		const char* propertyName, float& value) const;
	edict_t* getCurrentBootstrapEdict() const;
	void resetControlledTestState();

	bool m_bBootstrapOnly = false;
	bool m_bControllerProbeOnly = false;
	EmpiresControllerProbeLifecycle m_eControllerProbeLifecycle =
		EmpiresControllerProbeLifecycle::INACTIVE;
	EmpiresControllerProbeSpawnSequenceState m_eControllerProbeSpawnSequenceState =
		EmpiresControllerProbeSpawnSequenceState::IDLE;
	EmpiresControllerProbeSpawnHoldState m_eControllerProbeSpawnHoldState =
		EmpiresControllerProbeSpawnHoldState::IDLE;
	EmpiresControllerProbeNeutralReadinessState m_eControllerProbeNeutralReadinessState =
		EmpiresControllerProbeNeutralReadinessState::IDLE;
	EmpiresControllerProbeLoadoutReadinessState m_eControllerProbeLoadoutReadinessState =
		EmpiresControllerProbeLoadoutReadinessState::IDLE;
	EmpiresControllerProbeAppliedLoadoutReadinessState
		m_eControllerProbeAppliedLoadoutReadinessState =
		EmpiresControllerProbeAppliedLoadoutReadinessState::IDLE;
	EmpiresControllerProbeSpawnConfirmReadyState
		m_eControllerProbeSpawnConfirmReadyState =
		EmpiresControllerProbeSpawnConfirmReadyState::IDLE;
	bool m_bControllerProbeReadinessOnly = false;
	int m_iControllerProbeSlot = -1;
	int m_iControllerProbeClientIndex = -1;
	int m_iControllerProbePathTeam = -1;
	int m_iControllerProbeSequenceExpectedIndex = -1;
	int m_iControllerProbeSequenceExpectedUserId = -1;
	int m_iControllerProbeSequenceStageFrames = 0;
	int m_iControllerProbeSequencePressCount = 0;
	int m_iControllerProbeSequenceReleaseCount = 0;
	int m_iControllerProbeSequenceStableSamples = 0;
	bool m_bControllerProbeSequenceInitialAlive = false;
	Vector m_vControllerProbeSequenceOrigin;
	bool m_bControllerProbeSequenceOriginKnown = false;
	int m_iControllerProbeHoldExpectedIndex = -1;
	int m_iControllerProbeHoldExpectedUserId = -1;
	const edict_t* m_pControllerProbeHoldExpectedEdict = nullptr;
	int m_iControllerProbeHoldStageFrames = 0;
	int m_iControllerProbeHoldObserveFrames = 0;
	int m_iControllerProbeHoldAttackCount = 0;
	int m_iControllerProbeHoldReleaseCount = 0;
	bool m_bControllerProbeHoldTimerKnownEverySample = true;
	bool m_bControllerProbeHoldTimerFutureObserved = false;
	bool m_bControllerProbeHoldTimerExpired = false;
	bool m_bControllerProbeReadinessStartTimeKnown = false;
	float m_flControllerProbeReadinessStartTime = 0.0f;
	bool m_bControllerProbeReadinessLastLifeStateKnown = false;
	int m_iControllerProbeReadinessLastLifeState = -1;
	bool m_bControllerProbeReadinessLastTimerKnown = false;
	float m_flControllerProbeReadinessLastTimer = 0.0f;
	bool m_bControllerProbeReadinessLatestDeadlineKnown = false;
	float m_flControllerProbeReadinessLatestDeadline = 0.0f;
	int m_iControllerProbeNeutralExpectedIndex = -1;
	int m_iControllerProbeNeutralExpectedUserId = -1;
	const edict_t* m_pControllerProbeNeutralExpectedEdict = nullptr;
	int m_iControllerProbeNeutralStageFrames = 0;
	int m_iControllerProbeNeutralSubmittedCommands = 0;
	int m_iControllerProbeNeutralMaxCommands = 0;
	int m_iControllerProbeNeutralLastSubmittedTick = -1;
	int m_iControllerProbeNeutralLastSubmittedCommand = 0;
	int m_iControllerProbeNeutralLastProgressLogCommand = 0;
	bool m_bControllerProbeNeutralStartTimeKnown = false;
	float m_flControllerProbeNeutralStartTime = 0.0f;
	bool m_bControllerProbeNeutralLastLifeStateKnown = false;
	int m_iControllerProbeNeutralLastLifeState = -1;
	bool m_bControllerProbeNeutralLastTimerKnown = false;
	float m_flControllerProbeNeutralLastTimer = 0.0f;
	bool m_bControllerProbeNeutralLatestDeadlineKnown = false;
	float m_flControllerProbeNeutralLatestDeadline = 0.0f;
	int m_iControllerProbeLoadoutExpectedIndex = -1;
	int m_iControllerProbeLoadoutExpectedUserId = -1;
	const edict_t* m_pControllerProbeLoadoutExpectedEdict = nullptr;
	int m_iControllerProbeLoadoutStageFrames = 0;
	int m_iControllerProbeLoadoutSubmittedCommands = 0;
	int m_iControllerProbeLoadoutMaxCommands = 0;
	int m_iControllerProbeLoadoutLastSubmittedTick = -1;
	int m_iControllerProbeLoadoutLastSubmittedCommand = 0;
	int m_iControllerProbeLoadoutLastProgressLogCommand = 0;
	bool m_bControllerProbeLoadoutClassFirst = false;
	bool m_bControllerProbeLoadoutStartTimeKnown = false;
	float m_flControllerProbeLoadoutStartTime = 0.0f;
	bool m_bControllerProbeLoadoutNeutralStartTimeKnown = false;
	float m_flControllerProbeLoadoutNeutralStartTime = 0.0f;
	bool m_bControllerProbeLoadoutLastLifeStateKnown = false;
	int m_iControllerProbeLoadoutLastLifeState = -1;
	bool m_bControllerProbeLoadoutLastTimerKnown = false;
	float m_flControllerProbeLoadoutLastTimer = 0.0f;
	bool m_bControllerProbeLoadoutLatestDeadlineKnown = false;
	float m_flControllerProbeLoadoutLatestDeadline = 0.0f;
	int m_iControllerProbeAppliedLoadoutExpectedIndex = -1;
	int m_iControllerProbeAppliedLoadoutExpectedUserId = -1;
	const edict_t* m_pControllerProbeAppliedLoadoutExpectedEdict = nullptr;
	int m_iControllerProbeAppliedLoadoutStageFrames = 0;
	int m_iControllerProbeAppliedLoadoutSubmittedCommands = 0;
	int m_iControllerProbeAppliedLoadoutMaxCommands = 0;
	int m_iControllerProbeAppliedLoadoutLastSubmittedTick = -1;
	int m_iControllerProbeAppliedLoadoutLastSubmittedCommand = 0;
	int m_iControllerProbeAppliedLoadoutLastProgressLogCommand = 0;
	bool m_bControllerProbeAppliedLoadoutStartTimeKnown = false;
	float m_flControllerProbeAppliedLoadoutStartTime = 0.0f;
	bool m_bControllerProbeAppliedLoadoutNeutralStartTimeKnown = false;
	float m_flControllerProbeAppliedLoadoutNeutralStartTime = 0.0f;
	bool m_bControllerProbeAppliedLoadoutLastStateKnown = false;
	EmpiresBootstrapPlayerState m_controllerProbeAppliedLoadoutLastState;
	bool m_bControllerProbeAppliedLoadoutBeforeEquipmentKnown = false;
	EmpiresBootstrapPlayerState m_controllerProbeAppliedLoadoutBeforeEquipment;
	int m_iControllerProbeSpawnConfirmReadyExpectedIndex = -1;
	int m_iControllerProbeSpawnConfirmReadyExpectedUserId = -1;
	const edict_t* m_pControllerProbeSpawnConfirmReadyExpectedEdict = nullptr;
	int m_iControllerProbeSpawnConfirmReadyPressCount = 0;
	int m_iControllerProbeSpawnConfirmReadyReleaseCount = 0;
	int m_iControllerProbeSpawnConfirmReadyObserveFrames = 0;
	int m_iControllerProbeSpawnConfirmReadyLastSubmittedCommand = 0;
	int m_iControllerProbeSpawnConfirmReadyLastSubmittedTick = -1;
	int m_iControllerProbeSpawnConfirmReadyLastLifeState = -1;
	bool m_bControllerProbeSpawnConfirmReadyLastLifeStateKnown = false;
	Vector m_vControllerProbeSpawnConfirmReadyOrigin;
	bool m_bControllerProbeSpawnConfirmReadyOriginKnown = false;
	EmpiresControllerProbeMovementState m_eControllerProbeMovementState =
		EmpiresControllerProbeMovementState::IDLE;
	EmpiresControllerProbeWaypointState m_eControllerProbeWaypointState =
		EmpiresControllerProbeWaypointState::IDLE;
	int m_iControllerProbeMovementExpectedIndex = -1;
	int m_iControllerProbeMovementExpectedUserId = -1;
	const edict_t* m_pControllerProbeMovementExpectedEdict = nullptr;
	int m_iControllerProbeMovementActiveFrames = 0;
	int m_iControllerProbeMovementFinalObserveFrames = 0;
	int m_iControllerProbeMovementSubmittedCommands = 0;
	int m_iControllerProbeMovementLastSubmittedCommand = 0;
	int m_iControllerProbeMovementLastSubmittedTick = -1;
	bool m_bControllerProbeMovementStartOriginKnown = false;
	Vector m_vControllerProbeMovementStartOrigin;
	bool m_bControllerProbeMovementFinalOriginKnown = false;
	Vector m_vControllerProbeMovementFinalOrigin;
	QAngle m_vControllerProbeMovementViewAngles;
	int m_iControllerProbeWaypointExpectedIndex = -1;
	int m_iControllerProbeWaypointExpectedUserId = -1;
	const edict_t* m_pControllerProbeWaypointExpectedEdict = nullptr;
	int m_iControllerProbeWaypointId = -1;
	int m_iControllerProbePathStartWaypoint = -1;
	int m_iControllerProbePathDestinationWaypoint = -1;
	bool m_bControllerProbeWaypointRouteMode = false;
	bool m_bControllerProbeWaypointPathMode = false;
	bool m_bControllerProbeWaypointPathSearchStarted = false;
	int m_iControllerProbeWaypointPathSearchFrames = 0;
	int m_iControllerProbeWaypointPathNodeIndex = 0;
	std::vector<int> m_ControllerProbeWaypointPath;
	int m_iControllerProbeWaypointRouteTargetsVisited = 0;
	int m_iControllerProbeWaypointRoutePreviousId = -1;
	int m_iControllerProbeWaypointRouteSegmentFrames = 0;
	int m_iControllerProbeWaypointActiveFrames = 0;
	int m_iControllerProbeWaypointFinalObserveFrames = 0;
	int m_iControllerProbeWaypointSubmittedCommands = 0;
	int m_iControllerProbeWaypointLastSubmittedCommand = 0;
	int m_iControllerProbeWaypointLastSubmittedTick = -1;
	int m_iControllerProbeWaypointNoProgressFrames = 0;
	bool m_bControllerProbeWaypointInputActive = false;
	bool m_bControllerProbeWaypointStartOriginKnown = false;
	Vector m_vControllerProbeWaypointStartOrigin;
	QAngle m_vControllerProbeWaypointViewAngles;
	bool m_bControllerProbeWaypointTargetKnown = false;
	Vector m_vControllerProbeWaypointTargetOrigin;
	bool m_bControllerProbeWaypointFinalOriginKnown = false;
	Vector m_vControllerProbeWaypointFinalOrigin;
	float m_flControllerProbeWaypointStartTime = 0.0f;
	float m_flControllerProbeWaypointInitialDistance = 0.0f;
	float m_flControllerProbeWaypointBestDistance = 0.0f;
	bool m_bControllerProbeWaypointReached = false;
	bool m_bControllerProbeWaypointTimedOut = false;
	bool m_bControllerProbeWaypointNoProgress = false;
	bool m_bThinkGateLogged = false;
	bool m_bStartGameGateLogged = false;
	// Normal Empires bots start unassigned. Keep team selection explicit and
	// wait for the public team readback before generic bot thinking continues.
	bool m_bNormalTeamStartComplete = false;
	bool m_bNormalTeamCommandPending = false;
	bool m_bNormalTeamStartFailed = false;
	int m_iNormalTeam = -1;
	int m_iNormalTeamFrames = 0;
	float m_flNormalTeamDeadline = 0.0f;
	const edict_t* m_pNormalTeamEdict = nullptr;
	bool m_bNormalClassStartComplete = false;
	bool m_bNormalClassCommandPending = false;
	bool m_bNormalClassStartFailed = false;
	bool m_bNormalLoadoutRequested = false;
	bool m_bNormalLoadoutAccepted = false;
	int m_iNormalClass = -1;
	int m_iNormalClassFrames = 0;
	float m_flNormalClassDeadline = 0.0f;
	EmpiresNormalSpawnState m_eNormalSpawnState = EmpiresNormalSpawnState::IDLE;
	int m_iNormalSpawn = -1;
	int m_iNormalSpawnFrames = 0;
	float m_flNormalSpawnDeadline = 0.0f;
	float m_flNormalSpawnObserveDeadline = 0.0f;
	float m_flNormalSpawnConfirmEarliestTime = 0.0f;
	int m_iNormalSpawnExpectedIndex = -1;
	int m_iNormalSpawnExpectedUserId = -1;
	const edict_t* m_pNormalSpawnExpectedEdict = nullptr;
	// Diagnostic identity of the map spawn selected for the current normal
	// lifecycle.  This is observed only; it never changes spawn behavior.
	const edict_t* m_pNormalSpawnSelectionEdict = nullptr;
	bool m_bNormalSpawnEligibilityLogged = false;
	bool m_bNormalSpawnLastLifeStateKnown = false;
	int m_iNormalSpawnLastLifeState = -1;
	bool m_bNormalSpawnLastTimerKnown = false;
	float m_flNormalSpawnLastTimer = 0.0f;
	bool m_bNormalStartGameEntryLogged = false;
	bool m_bNormalThinkObserverEntryLogged = false;
	// Engine reuses the same edict address after a bot is kicked. Track the
	// user id as the client-generation identity so a fresh bot cannot inherit
	// the previous normal lifecycle/path state.
	int m_iNormalLifecycleUserId = -1;
	// player_death is the earliest reliable normal-bot lifecycle signal. It is
	// consumed by suppressBotThink(); live-state polling remains the fallback.
	bool m_bNormalDeathEventPending = false;
	EmpiresNormalPathState m_eNormalPathState = EmpiresNormalPathState::IDLE;
	EmpiresVehicleActionState m_eEmpiresVehicleActionState =
		EmpiresVehicleActionState::IDLE;
	int m_iEmpiresVehicleActionExpectedIndex = -1;
	int m_iEmpiresVehicleActionExpectedUserId = -1;
	const edict_t* m_pEmpiresVehicleActionExpectedEdict = nullptr;
	MyEHandle m_pEmpiresVehicleActionFactory;
	MyEHandle m_pEmpiresVehicleActionTutorialExit;
	int m_iEmpiresVehicleActionTutorialExitIndex = -1;
	Vector m_vEmpiresVehicleActionTutorialExitOrigin = vec3_origin;
	Vector m_vEmpiresVehicleActionTutorialEntryOrigin = vec3_origin;
	float m_flEmpiresVehicleActionTutorialTeleportDeadline = 0.0f;
	int m_iEmpiresVehicleActionFactoryIndex = -1;
	int m_iEmpiresVehicleActionFactoryTeam = -1;
	Vector m_vEmpiresVehicleActionFactoryOrigin = vec3_origin;
	int m_iEmpiresVehicleActionPurchaseStep = 0;
	float m_flEmpiresVehicleActionNextPurchaseCommand = 0.0f;
	std::vector<int> m_EmpiresVehicleActionBaselineVehicleIndices;
	MyEHandle m_pEmpiresVehicleActionVehicle;
	int m_iEmpiresVehicleActionVehicleIndex = -1;
	int m_iEmpiresVehicleActionVehicleTeam = -1;
	int m_iEmpiresVehicleActionPassenger0 = -1;
	int m_iEmpiresVehicleActionFrames = 0;
	float m_flEmpiresVehicleActionDeadline = 0.0f;
	float m_flEmpiresVehicleActionDriveStart = 0.0f;
	Vector m_vEmpiresVehicleActionStartOrigin = vec3_origin;
	Vector m_vEmpiresVehicleActionLastOrigin = vec3_origin;
	QAngle m_qEmpiresVehicleActionDriveAngles = vec3_angle;
	bool m_bEmpiresVehicleActionMoved = false;
	bool m_bEmpiresVehicleActionUseLogged = false;
	bool m_bEmpiresVehicleActionExitUseLogged = false;
	bool m_bNormalPathAutoStarted = false;
	// Manual path_once remains a terminal regression test. The normal lifecycle
	// route must return to CBot::think() so an observed enemy can be approached.
	bool m_bNormalPathTerminalAfterStop = false;
	bool m_bNormalPathSearchStarted = false;
	int m_iNormalPathSearchFrames = 0;
	int m_iNormalPathStartWaypoint = -1;
	int m_iNormalPathDestinationWaypoint = -1;
	int m_iNormalPathLastWaypoint = -1;
	int m_iNormalPathActiveFrames = 0;
	int m_iNormalPathNoProgressFrames = 0;
	int m_iNormalPathExpectedIndex = -1;
	int m_iNormalPathExpectedUserId = -1;
	const edict_t* m_pNormalPathExpectedEdict = nullptr;
	float m_flNormalPathStartTime = 0.0f;
	float m_flNormalPathDeadline = 0.0f;
	Vector m_vNormalPathStartOrigin;
	Vector m_vNormalPathLastOrigin;
	Vector m_vNormalPathDestinationOrigin;
	bool m_bNormalPathLastOriginKnown = false;
	std::vector<int> m_NormalPathRoute;
	// One current visible Empires enemy may own one normal CFindPathTask. This
	// is deliberately a small bridge to existing RCBot2 scheduling, not a new
	// combat or navigation system.
	MyEHandle m_pEmpiresApproachTarget;
	bool m_bEmpiresApproachTaskIssued = false;
	// Diagnostic-only throttle for the combat-relevance IGNORE log (engine
	// time of the next permitted log). Never consulted by any decision: the
	// relevance predicates are pure functions of live inputs, so a reset
	// can only restore identical decisions (see REL-X1).
	float m_flEmpiresRelevanceIgnoreLogTime = 0.0f;
	MyEHandle m_pEmpiresLastSpotTarget;
	float m_flEmpiresNextSpotTime = 0.0f;
	bool m_bEmpiresLastEnemyObscured = false;
	bool m_bEmpiresLastEnemySearchActive = false;
	bool m_bEmpiresLastEnemySearchTaskLogged = false;
	bool m_bEmpiresCombatEngaged = false;
	bool m_bEmpiresCoverScheduleActive = false;
	bool m_bEmpiresCaptureApproachIssued = false;
	bool m_bEmpiresCaptureHoldIssued = false;
	bool m_bEmpiresEngineerSupportTaskIssued = false;
	bool m_bEmpiresEngineerSupportNearby = false;
	bool m_bEmpiresEngineerResupplyTaskIssued = false;
	int m_iEmpiresCaptureApproachPoint = -1;
	int m_iEmpiresCaptureRouteWaypoint = -1;
	int m_iEmpiresCaptureApproachRetries = 0;
	// One-shot cause marker: tactical combat removed the live capture
	// schedule. The next capture accounting decision consumes it and resumes
	// a still-valid objective without spending retry budget.
	RCBotEmpiresCaptureRetry::CaptureInterruptionLatch m_captureInterruptionLatch;
	// Bounded start-anchor recovery for capture drains that fail anchoring
	// before any useful route exists (NO_START_ANCHOR, zero progress). One
	// re-anchor leg per capture point; cleared with all other capture state
	// on teardown, death, setup, fixture release, recovery, or map-memory
	// free so stale recovery can never affect a later objective.
	RCBotEmpiresStartAnchorRecovery::StartAnchorRecoveryState
		m_empiresStartAnchorRecovery;
	int m_iEmpiresStartAnchorRecoveryWaypoint = -1;
	Vector m_vEmpiresStartAnchorRecoveryStartOrigin;
	float m_flEmpiresStartAnchorRecoveryStartTime = 0.0f;
	// Per-bot bounded memory for a capture route that exhausted its retries
	// before arrival; it prevents immediate same-point arbitration churn.
	int m_iEmpiresCaptureRejectedPoint = -1;
	int m_iEmpiresEngineerSupportBuildingIndex = -1;
	int m_iEmpiresEngineerSupportWaypoint = -1;
	// A routed support waypoint that failed to reach is temporarily excluded
	// for this bot so the next normal arbitration can try another valid anchor
	// for the same building instead of repeating the identical dead route.
	int m_iEmpiresEngineerSupportRejectedBuildingIndex = -1;
	int m_iEmpiresEngineerSupportRejectedWaypoint = -1;
	int m_iEmpiresEngineerResupplyWaypoint = -1;
	Vector m_vEmpiresCaptureApproachOrigin;
	Vector m_vEmpiresCaptureApproachStartOrigin;
	Vector m_vEmpiresEngineerSupportWaypointOrigin;
	Vector m_vEmpiresEngineerResupplyWaypointOrigin;
	float m_flEmpiresNextCaptureApproachTime = 0.0f;
	float m_flEmpiresNextCaptureApproachProgressLogTime = 0.0f;
	float m_flEmpiresCaptureHoldDeadline = 0.0f;
	float m_flEmpiresCaptureRejectedUntil = 0.0f;
	// Diagnostic-only: engine time when the current capture route attempt was
	// issued (fresh selection or retry). Gates the per-bot task-terminal slot
	// so the CAPTURE_ROUTE_TERMINAL event only reports terminals recorded
	// since this attempt. Never branches behavior. -1 when no route active.
	float m_flEmpiresCaptureRouteIssuedTime = -1.0f;
	RCBotRecovery::EpisodeTracker m_empireRecoveryEpisodeTracker;
	RCBotRecovery::RecoveryEpisodePolicy m_empireRecoveryEpisodePolicy;
	std::uint64_t m_iEmpiresCaptureObjectiveGeneration = 0;
	// Diagnostic-only identity for each issued capture route. It is distinct
	// from objective generation: a same-objective combat resume gets a new
	// route attempt while retaining the objective episode.
	std::uint64_t m_iEmpiresCaptureRouteAttemptGeneration = 0;
	int m_iEmpiresRecoveryCapturePoint = -1;
	int m_iEmpiresRecoveryCaptureProgress = -1;
	int m_iEmpiresRecoveryLastTeam = -1;
	int m_iEmpiresRecoverySquadTeam = -1;
	int m_iEmpiresRecoverySquad = -1;
	int m_iEmpiresRecoverySquadGeneration = -1;
	int m_iEmpiresRecoverySquadWaypoint = -1;
	float m_flEmpiresNextEngineerSupportTime = 0.0f;
	float m_flEmpiresEngineerSupportRejectedUntil = 0.0f;
	float m_flEmpiresNextEngineerResupplyTime = 0.0f;
	// Cooldown for the bounded Grenadier action that can follow a completed
	// last-known-enemy search. The existing mine action owns all input/state.
	float m_flEmpiresNextLastKnownMineTime = 0.0f;
	// Cooldown for the bounded visible-enemy Grenadier mortar tactic. The
	// existing mortar action owns all input/state after the request is accepted.
	float m_flEmpiresNextVisibleMortarTime = 0.0f;
	float m_flEmpiresReactionUntil = 0.0f;
	EmpiresMortarProbeState m_eEmpiresMortarProbeState = EmpiresMortarProbeState::IDLE;
	int m_iEmpiresMortarProbeAttempt = 0;
	int m_iEmpiresMortarProbeFrames = 0;
	int m_iEmpiresMortarProbeShellBaseline = 0;
	int m_iEmpiresMortarProbeShellIndex = -1;
	int m_iEmpiresMortarProbeLastAmmo = -1;
	int m_iEmpiresMortarProbeLastClip = -1;
	int m_iEmpiresMortarProbeLastFlags = 0;
	bool m_bEmpiresMortarProbeLastFlagsKnown = false;
	bool m_bEmpiresMortarProbePostureReadyLogged = false;
	bool m_bEmpiresMortarProbeLoadedKnown = false;
	bool m_bEmpiresMortarProbeLastLoaded = false;
	bool m_bEmpiresMortarProbeReloadTimeKnown = false;
	float m_flEmpiresMortarProbeReloadTime = 0.0f;
	bool m_bEmpiresMortarProbeNextPrimaryKnown = false;
	float m_flEmpiresMortarProbeNextPrimaryAttack = 0.0f;
	bool m_bEmpiresMortarProbeInitialNeutralLogged = false;
	bool m_bEmpiresMortarProbeReleaseLogged = false;
	int m_iEmpiresMortarProbeTelemetryPending = 0;
	float m_flEmpiresMortarProbeDeadline = 0.0f;
	float m_flEmpiresMortarProbeShellDeadline = 0.0f;
	int m_iEmpiresMortarProbeExpectedIndex = -1;
	int m_iEmpiresMortarProbeExpectedUserId = -1;
	const edict_t* m_pEmpiresMortarProbeExpectedEdict = nullptr;
	const edict_t* m_pEmpiresMortarProbeWeapon = nullptr;
	EmpiresMortarActionState m_eEmpiresMortarActionState =
		EmpiresMortarActionState::IDLE;
	int m_iEmpiresMortarActionTeam = -1;
	int m_iEmpiresMortarActionExpectedIndex = -1;
	int m_iEmpiresMortarActionExpectedUserId = -1;
	const edict_t* m_pEmpiresMortarActionExpectedEdict = nullptr;
	const edict_t* m_pEmpiresMortarActionWeapon = nullptr;
	int m_iEmpiresMortarActionShellBaseline = 0;
	int m_iEmpiresMortarActionShellIndex = -1;
	int m_iEmpiresMortarActionAmmoBefore = -1;
	int m_iEmpiresMortarActionAmmoAfter = -1;
	int m_iEmpiresMortarActionFrames = 0;
	bool m_bEmpiresMortarActionPostureLogged = false;
	bool m_bEmpiresMortarActionAttackLogged = false;
	bool m_bEmpiresMortarActionReleaseLogged = false;
	float m_flEmpiresMortarActionDeadline = 0.0f;
	float m_flEmpiresMortarActionShellDeadline = 0.0f;
	EmpiresEngineerRepairActionState m_eEmpiresEngineerRepairActionState =
		EmpiresEngineerRepairActionState::IDLE;
	int m_iEmpiresEngineerRepairActionTeam = -1;
	int m_iEmpiresEngineerRepairActionExpectedIndex = -1;
	int m_iEmpiresEngineerRepairActionExpectedUserId = -1;
	const edict_t* m_pEmpiresEngineerRepairActionExpectedEdict = nullptr;
	const edict_t* m_pEmpiresEngineerRepairActionBuilding = nullptr;
	int m_iEmpiresEngineerRepairActionBuildingIndex = -1;
	int m_iEmpiresEngineerRepairActionHealthBefore = -1;
	int m_iEmpiresEngineerRepairActionHealthLast = -1;
	int m_iEmpiresEngineerRepairActionMaxHealth = -1;
	int m_iEmpiresEngineerRepairActionFrames = 0;
	float m_flEmpiresEngineerRepairActionDeadline = 0.0f;
	float m_flEmpiresEngineerRepairApproachLast = 0.0f;  // view-ray distance last frame while walking in
	bool m_bEmpiresEngineerRepairActionRequireFullHealth = false;
	bool m_bEmpiresEngineerRepairActionProgressLogged = false;
	EmpiresEngineerBuildSelectionState m_eEmpiresEngineerBuildSelectionState =
		EmpiresEngineerBuildSelectionState::IDLE;
	int m_iEmpiresEngineerBuildSelectionTeam = -1;
	int m_iEmpiresEngineerBuildSelectionExpectedIndex = -1;
	int m_iEmpiresEngineerBuildSelectionExpectedUserId = -1;
	const edict_t* m_pEmpiresEngineerBuildSelectionExpectedEdict = nullptr;
	const edict_t* m_pEmpiresEngineerBuildSelectionWeapon = nullptr;
	int m_iEmpiresEngineerBuildSelectionFrames = 0;
	int m_iEmpiresEngineerBuildSelectionBaselineItem = -1;
	int m_iEmpiresEngineerBuildSelectionLastItem = -1;
	int m_iEmpiresEngineerBuildSelectionBaselineState = -1;
	int m_iEmpiresEngineerBuildSelectionLastState = -1;
	bool m_bEmpiresEngineerBuildSelectionItemKnown = false;
	bool m_bEmpiresEngineerBuildSelectionStateKnown = false;
	bool m_bEmpiresEngineerBuildSelectionInputRequested = false;
	bool m_bEmpiresEngineerBuildSelectionInputSubmitted = false;
	bool m_bEmpiresEngineerBuildSelectionBaselineLogged = false;
	bool m_bEmpiresEngineerBuildSelectionTransitionLogged = false;
	float m_flEmpiresEngineerBuildSelectionDeadline = 0.0f;
	EmpiresEngineerBuildActionState m_eEmpiresEngineerBuildActionState =
		EmpiresEngineerBuildActionState::IDLE;
	int m_iEmpiresEngineerBuildActionTeam = -1;
	int m_iEmpiresEngineerBuildActionExpectedIndex = -1;
	int m_iEmpiresEngineerBuildActionExpectedUserId = -1;
	const edict_t* m_pEmpiresEngineerBuildActionExpectedEdict = nullptr;
	int m_iEmpiresEngineerBuildActionTargetWaypoint = -1;
	int m_iEmpiresEngineerBuildActionBaselineTurrets = 0;
	int m_iEmpiresEngineerBuildActionBaselineMaxIndex = -1;
	int m_iEmpiresEngineerBuildActionResultIndex = -1;
	int m_iEmpiresEngineerBuildActionFrames = 0;
	bool m_bEmpiresEngineerBuildActionCommandRequested = false;
	bool m_bEmpiresEngineerBuildActionCompletionCommandRequested = false;
	bool m_bEmpiresEngineerBuildActionAttackSubmitted = false;
	bool m_bEmpiresEngineerBuildActionBaselineLogged = false;
	bool m_bEmpiresEngineerBuildActionSelectionLogged = false;
	bool m_bEmpiresEngineerBuildActionResultObservedLogged = false;
	bool m_bEmpiresEngineerBuildActionRequireFullHealth = false;
	int m_iEmpiresEngineerBuildActionLastTraceIndex = -2;
	int m_iEmpiresEngineerBuildActionLastTraceOwner = -2;
	int m_iEmpiresEngineerBuildActionLastTraceMoveParent = -2;
	int m_iEmpiresEngineerBuildActionLastTraceParent = -2;
	int m_iEmpiresEngineerBuildActionLastTraceInfoOwner = -2;
	int m_iEmpiresEngineerBuildActionLastTraceInfoParent = -2;
	int m_iEmpiresEngineerBuildActionLastTraceInfoMoveParent = -2;
	int m_iEmpiresEngineerBuildActionLastTraceInfoRootMoveParent = -2;
	int m_iEmpiresEngineerBuildActionLastTraceHealth = -2;
	int m_iEmpiresEngineerBuildActionLastTraceMaxHealth = -2;
	int m_iEmpiresEngineerBuildActionLastTraceFullyBuilt = -2;
	Vector m_vEmpiresEngineerBuildActionTarget = vec3_origin;
	float m_flEmpiresEngineerBuildActionDeadline = 0.0f;
	bool m_bEmpiresCommanderMoveTestActive = false;
	Vector m_vEmpiresCommanderMoveTestTarget = vec3_origin;
	bool m_bEmpiresCommanderMoveRouteIssued = false;
	bool m_bEmpiresCommanderMoveFinished = false;
	bool m_bEmpiresCommanderMoveFailed = false;
	bool m_bEmpiresCommanderMoveSynthetic = false;
	bool m_bEmpiresCommanderMoveRetrying = false;
	bool m_bEmpiresCommanderMoveStuck = false;
	int m_iEmpiresCommanderMoveRetries = 0;
	float m_fEmpiresCommanderMoveRetryAt = 0.0f;
	Vector m_vEmpiresCommanderMoveTarget = vec3_origin;
	int m_iEmpiresCommanderMoveGeneration = 0;
	std::uint64_t m_iEmpiresCommanderMoveNavigationIntentId = 0;
	bool m_bEmpiresSquadSpatialChildActive = false;
	RCBotOrder::OrderKey m_EmpiresSquadSpatialParentOrder;
	RCBotOrder::OrderKey m_EmpiresSquadSpatialChildExecution;
	RCBotOrder::RecipientId m_iEmpiresSquadSpatialMemberId = 0;
	std::uint64_t m_iEmpiresSquadSpatialRequestToken = 0;
	std::uint64_t m_iEmpiresSquadSpatialNavigationIntentId = 0;
	std::uint64_t m_iEmpiresSquadSpatialSlotId = 0;
	RCBotOrder::Generation m_iEmpiresSquadSpatialGeneration = 0;
	std::uint64_t m_iEmpiresSquadSpatialDecisionRevision = 0;
	RCBotOrder::Generation m_iEmpiresSquadSpatialFormationGeneration = 0;
	std::uint8_t m_iEmpiresSquadSpatialClaimKind = 0;
	std::uint64_t m_iEmpiresSquadSpatialClaimId = 0;
	RCBotOrder::Generation m_iEmpiresSquadSpatialClaimGeneration = 0;
	Vector m_vEmpiresSquadSpatialTarget = vec3_origin;
	Vector m_vEmpiresCommanderMovePreviousTarget = vec3_origin;
	float m_flEmpiresCommanderMoveNextDiagnosticLog = 0.0f;
	float m_flEmpiresCommanderMoveNextCommandDiagnosticLog = 0.0f;
	bool m_bEmpiresCommanderMoveDiagnosticCommandPending = false;
	int m_iEmpiresCommanderMoveDiagnosticCommandNumber = -1;
	RCBotCommanderOrder::CommanderMoveOrderAdapter m_commanderMoveShadow;
	EmpiresEngineerResupplyActionState m_eEmpiresEngineerResupplyActionState =
		EmpiresEngineerResupplyActionState::IDLE;
	int m_iEmpiresEngineerResupplyActionTeam = -1;
	int m_iEmpiresEngineerResupplyActionExpectedIndex = -1;
	int m_iEmpiresEngineerResupplyActionExpectedUserId = -1;
	const edict_t* m_pEmpiresEngineerResupplyActionExpectedEdict = nullptr;
	const edict_t* m_pEmpiresEngineerResupplyActionSource = nullptr;
	int m_iEmpiresEngineerResupplyActionSourceIndex = -1;
	Vector m_vEmpiresEngineerResupplyActionSourceOrigin = vec3_origin;
	Vector m_vEmpiresEngineerResupplyActionFixtureOrigin = vec3_origin;
	bool m_bEmpiresEngineerResupplyActionFixturePositionCorrected = false;
	bool m_bEmpiresEngineerResupplyActionFixturePositionDriftLogged = false;
	bool m_bEmpiresEngineerResupplyActionFixtureMode = true;
	// Development-only precondition probe. It consumes the current primary
	// through normal input and then hands control back to normal getTasks().
	bool m_bEmpiresEngineerResupplyActionDepletionOnly = false;
	// One-shot diagnostic for the handoff from the bounded depletion probe to
	// the normal Engineer resupply selector.
	bool m_bEmpiresEngineerResupplyHandoffCheckLogged = false;
	int m_iEmpiresEngineerResupplyActionAmmoType = -1;
	int m_iEmpiresEngineerResupplyActionAmmoBefore = -1;
	int m_iEmpiresEngineerResupplyActionAmmoLast = -1;
	int m_iEmpiresEngineerResupplyActionClipBefore = -1;
	int m_iEmpiresEngineerResupplyActionClipLast = -1;
	int m_iEmpiresEngineerResupplyActionFrames = 0;
	bool m_bEmpiresEngineerResupplyActionUseSubmitted = false;
	bool m_bEmpiresEngineerResupplyActionUseInputLogged = false;
	bool m_bEmpiresEngineerResupplyActionUsePreRunLogged = false;
	bool m_bEmpiresEngineerResupplyActionUseProcessedLogged = false;
	bool m_bEmpiresEngineerResupplyActionUseTraceBlockedLogged = false;
	bool m_bEmpiresEngineerResupplyActionBaselineLogged = false;
	bool m_bEmpiresEngineerResupplyActionDepletionAttackSubmitted = false;
	bool m_bEmpiresEngineerResupplyActionDepletionReadinessLogged = false;
	bool m_bEmpiresEngineerResupplyActionDepletionReloadSubmitted = false;
	bool m_bEmpiresEngineerResupplyActionDepletionComplete = false;
	float m_flEmpiresEngineerResupplyActionDeadline = 0.0f;
	EmpiresMineActionState m_eEmpiresMineActionState =
		EmpiresMineActionState::IDLE;
	int m_iEmpiresMineActionTeam = -1;
	int m_iEmpiresMineActionExpectedIndex = -1;
	int m_iEmpiresMineActionExpectedUserId = -1;
	const edict_t* m_pEmpiresMineActionExpectedEdict = nullptr;
	const edict_t* m_pEmpiresMineActionWeapon = nullptr;
	int m_iEmpiresMineActionProjectileBaseline = 0;
	int m_iEmpiresMineActionProjectileIndex = -1;
	int m_iEmpiresMineActionAmmoBefore = -1;
	int m_iEmpiresMineActionAmmoAfter = -1;
	int m_iEmpiresMineActionFrames = 0;
	int m_iEmpiresMineActionDeployObservations = 0;
	int m_iEmpiresMineActionSettledFrames = 0;
	int m_iEmpiresMineActionTriggerTargetUserId = -1;
	int m_iEmpiresMineActionTriggerTargetIndex = -1;
	int m_iEmpiresMineActionTriggerTargetHealthBefore = -1;
	int m_iEmpiresMineActionTriggerTargetHealthAfter = -1;
	const edict_t* m_pEmpiresMineActionTriggerTargetEdict = nullptr;
	bool m_bEmpiresMineActionTriggerMode = false;
	float m_flEmpiresMineActionTriggerDelaySeconds = 0.0f;
	bool m_bEmpiresMineActionTriggerPositionLogged = false;
	bool m_bEmpiresMineActionTriggerHoldLogged = false;
	bool m_bEmpiresMineActionTriggerImpactLogged = false;
	bool m_bEmpiresMineActionTriggerNaturalDeathLogged = false;
	bool m_bEmpiresMineActionAttackLogged = false;
	bool m_bEmpiresMineActionReleaseLogged = false;
	bool m_bEmpiresMineActionDeployLogged = false;
	int m_iEmpiresMineActionActivationLast = -1;
	bool m_bEmpiresMineActionActivationUnavailableLogged = false;
	bool m_bEmpiresMineActionActivationLogged = false;
	float m_flEmpiresMineActionDeployedTime = 0.0f;
	float m_flEmpiresMineActionDeadline = 0.0f;
	float m_flEmpiresMineActionProjectileDeadline = 0.0f;
	EmpiresScoutBinocularActionState m_eEmpiresScoutBinocularActionState =
		EmpiresScoutBinocularActionState::IDLE;
	int m_iEmpiresScoutBinocularActionExpectedIndex = -1;
	int m_iEmpiresScoutBinocularActionExpectedUserId = -1;
	const edict_t* m_pEmpiresScoutBinocularActionExpectedEdict = nullptr;
	const edict_t* m_pEmpiresScoutBinocularActionTargetEdict = nullptr;
	int m_iEmpiresScoutBinocularActionTargetIndex = -1;
	int m_iEmpiresScoutBinocularActionTargetUserId = -1;
	int m_iEmpiresScoutBinocularActionTargetTeam = -1;
	bool m_bEmpiresScoutBinocularActionTargetBuilding = false;
	const edict_t* m_pEmpiresScoutBinocularActionWeapon = nullptr;
	int m_iEmpiresScoutBinocularActionFrames = 0;
	float m_flEmpiresScoutBinocularActionDeadline = 0.0f;
	float m_flEmpiresScoutBinocularActionAimUntil = 0.0f;
	bool m_bEmpiresScoutBinocularActionAttackLogged = false;
	bool m_bEmpiresScoutBinocularActionReleaseLogged = false;
	bool m_bEmpiresScoutBinocularActionPropertyBaselineLogged = false;
	bool m_bEmpiresScoutBinocularActionSpotTransitionLogged = false;
	int m_iEmpiresScoutBinocularActionBaselineSpotter = -1;
	bool m_bEmpiresScoutBinocularActionBaselineSpotterKnown = false;
	bool m_bEmpiresScoutBinocularActionBaselineSpotted = false;
	bool m_bEmpiresScoutBinocularActionBaselineSpottedKnown = false;
	int m_iEmpiresScoutBinocularActionBaselineVisibilityState = -1;
	bool m_bEmpiresScoutBinocularActionBaselineVisibilityKnown = false;
	bool m_bEmpiresMortarAimRequested = false;
	bool m_bEmpiresMortarAimCheckArc = false;
	int m_iEmpiresMortarAimTargetWaypoint = -1;
	Vector m_vEmpiresMortarAimTarget = Vector(0.0f, 0.0f, 0.0f);
	QAngle m_qEmpiresMortarAimAngles = QAngle(0.0f, 0.0f, 0.0f);
	Vector m_vEmpiresMortarAimLaunchOrigin = Vector(0.0f, 0.0f, 0.0f);
	bool m_bEmpiresMortarAimComputed = false;
	bool m_bEmpiresMortarAimViewApplied = false;
	bool m_bEmpiresMortarAimProcessedLogged = false;
	int m_iEmpiresMortarAimTrajectorySamples = 0;
	float m_flEmpiresMortarAimLastSampleTime = 0.0f;
	Vector m_vEmpiresMortarAimFirstShellOrigin = Vector(0.0f, 0.0f, 0.0f);
	Vector m_vEmpiresMortarAimLastShellOrigin = Vector(0.0f, 0.0f, 0.0f);
	int m_iLastEmpiresPlayerObservation[RCBOT_MAXPLAYERS] = {};
	const edict_t* m_pLastEmpiresPlayerEdict[RCBOT_MAXPLAYERS] = {};
	int m_iLastEmpiresPlayerUserId[RCBOT_MAXPLAYERS] = {};
	int m_iEmpiresSquadObserved = -1;
	int m_iEmpiresSquadPositionObserved = -1;
	bool m_bEmpiresSquadLeaderObserved = false;
	bool m_bEmpiresSquadLeaderKnownObserved = false;
	float m_flEmpiresNextSquadActionTime = 0.0f;
	int m_iEmpiresSquadJoinAttempts = 0;
	bool m_bEmpiresSquadGoalTaskIssued = false;
	bool m_bEmpiresNavGoalActive = false;
	int m_iEmpiresNavGoalType = -1;
	int m_iEmpiresNavGoalEntity = -1;
	char m_szEmpiresNavGoalClass[48] = {};
	Vector m_vEmpiresNavGoalOrigin = Vector(0, 0, 0);
	float m_flEmpiresNavGoalStart = 0.0f;
	float m_flEmpiresNavGoalDeadline = 0.0f;
	float m_flEmpiresNavGoalHoldUntil = 0.0f;
	bool m_bEmpiresNavGoalArrivedAtCapture = false;
	// Grenadier mines around a defended point (max 8 live per player: a 9th
	// blows up the oldest).
	int m_iEmpiresDefendHoldEntity = -1;
	Vector m_vEmpiresDefendHoldOrigin;
	float m_flEmpiresDefendMineNext = 0.0f;
	float m_flEmpiresDefendMineAimUntil = 0.0f;
	bool m_bEmpiresDefendMineFullLogged = false;
	float m_flEmpiresNavGoalNextPick = 0.0f;
	int m_iEmpiresNavGoalRetries = 0;
	int m_iEmpiresNavGoalAttempts = 0;          // all retries, learned ones included
	int m_iEmpiresLastHealth = 0;               // modThink: notice damage without a player_hurt event
	int m_iEmpiresTurretThreat = 0;             // entity index of the turret that shot us
	Vector m_vEmpiresTurretThreatOrigin;
	float m_flEmpiresTurretThreatRange = 0.0f;
	float m_flEmpiresTurretThreatUntil = 0.0f;
	bool m_bEmpiresTurretRetreatPending = false;
	bool m_bEmpiresTurretRetreating = false;
	Vector m_vEmpiresTurretRetreatPoint;
	float m_flEmpiresTurretRetreatDeadline = 0.0f;
	MyEHandle m_hEmpiresTurretHitBy;
	MyEHandle m_hEmpiresTurretFireTarget;
	MyEHandle m_hEmpiresTurretRocketTarget;
	MyEHandle m_hEmpiresTurretRocketWeapon;
	QAngle m_qEmpiresTurretRocketAim;
	float m_flEmpiresTurretRocketUntil = 0.0f;
	int m_iEmpiresTurretRocketAmmoType = -1;
	int m_iEmpiresTurretRocketAmmo = 0;
	int m_iEmpiresTurretRocketHealth = 0;
	bool m_bEmpiresTurretRocketFired = false;
	bool m_bEmpiresTurretRocketApproach = false;

	// Pressing a map button that opens a closed door (as_escort base gate control1).
	int m_iEmpiresDoorButton = -1;
	int m_iEmpiresDoorButtonDoor = -1;
	Vector m_vEmpiresDoorButtonOrigin = Vector(0, 0, 0);
	float m_flEmpiresDoorButtonUntil = 0.0f;
	float m_flEmpiresDoorButtonNext = 0.0f;
	float m_flEmpiresDoorButtonLastUse = 0.0f;
	bool m_bEmpiresDoorButtonMoving = false;

	// Grenadier shelling a known turret from outside its range.
	float m_flEmpiresTurretMortarNext = 0.0f;
	float m_flEmpiresTurretFireMoveUntil = 0.0f;
	float m_flEmpiresTurretFireMoveNext = 0.0f;
	int m_iEmpiresTurretMortarFails = 0;
	int m_iEmpiresTurretMortarShots = 0;
	Vector m_vEmpiresTurretLastFireSpot = vec3_origin;
	Vector m_vEmpiresTurretFailedMortarSpot = vec3_origin;
	Vector m_vEmpiresTurretSettleOrigin;
	float m_flEmpiresTurretSettledSince = 0.0f;
	bool m_bEmpiresTurretMortarWasActive = false;
	bool m_bEmpiresTurretOutOfReachLogged = false;
	unsigned m_uEmpiresNavGoalBlockMark = 0;    // RCBotNavMesh::BlockGeneration at the last request
	unsigned m_iEmpiresNavGoalPicks = 0;
	struct EmpiresNavGoalAvoid { int entity = -1; float until = 0.0f; };
	std::vector<EmpiresNavGoalAvoid> m_EmpiresNavGoalAvoid;
	int m_iEmpiresNavGoalAvoidNext = 0;
	bool m_bEmpiresSquadGoalReached = false;
	int m_iEmpiresSquadGoalGeneration = -1;
	int m_iEmpiresSquadGoalWaypoint = -1;
	int m_iEmpiresSquadGoalLeaderUserId = -1;
	int m_iEmpiresSquadGoalRecoveryAttempts = 0;
	Vector m_vEmpiresSquadGoalOrigin = vec3_origin;
	Vector m_vEmpiresSquadGoalTarget = vec3_origin;
	bool m_bRemovalRequested = false;
	bool m_bTeamCommandPending = false;
	int m_iPendingTeam = -1;
	int m_iPendingTeamFrames = 0;
	float m_flPendingTeamDeadline = 0.0f;
	bool m_bClassCommandPending = false;
	int m_iPendingClass = -1;
	int m_iPendingClassFrames = 0;
	float m_flPendingClassDeadline = 0.0f;
	bool m_bPendingClassDesiredKnown = false;
	bool m_bPendingClassDesiredBefore = false;
	int m_iSelectedBootstrapClass = -1;
	bool m_bSpawnCommandPending = false;
	int m_iPendingSpawn = -1;
	int m_iPendingSpawnFrames = 0;
	float m_flPendingSpawnDeadline = 0.0f;
	Vector m_vSpawnOriginBefore;
	bool m_bSpawnOriginBeforeKnown = false;
	Vector m_vSpawnOriginAfter;
	bool m_bSpawnOriginAfterKnown = false;
	int m_iSpawnStableSamples = 0;
	EmpiresBootstrapSpawnConfirmationState m_eSpawnConfirmationState =
		EmpiresBootstrapSpawnConfirmationState::IDLE;
	int m_iSpawnConfirmationFrames = 0;
	int m_iSpawnConfirmationPressCount = 0;
	int m_iSpawnConfirmationReleaseCount = 0;
	int m_iSpawnConfirmationExpectedIndex = -1;
	int m_iSpawnConfirmationExpectedUserId = -1;
	const edict_t* m_pSpawnConfirmationExpectedEdict = nullptr;
	Vector m_vSpawnConfirmationOrigin;
	bool m_bSpawnConfirmationOriginKnown = false;
	int m_iSpawnConfirmationStableSamples = 0;
};

#endif // _BOT_EMPIRES_H_
