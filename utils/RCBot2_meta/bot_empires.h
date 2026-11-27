#ifndef _BOT_EMPIRES_H_
#define _BOT_EMPIRES_H_

#include "bot.h"
#include "bot_empires_look_logic.h"
#include "bot_empires_skirmish_logic.h"
#include "bot_empires_team_intel.h"
#include "bot_empires_capture_retry.h"
#include "bot_empires_commander_economy.h"
#include "bot_empires_vehicle_loadout.h"
#include "bot_empires_combat_relevance.h"
#include "bot_empires_combat_telemetry.h"
#include "bot_empires_start_anchor_recovery.h"
#include "bot_empires_commander_order_adapter.h"
#include "bot_empires_leader_actuation.h"
#include "bot_empires_vacancy_revive.h"
#include "bot_empires_priority.h"
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
void EmpiresDeployablesLevelReset();
int EmpiresPlayerTurretRestrict(edict_t* player);
void EmpiresReviveLevelReset();
void EmpiresVehicleLevelReset();
void EmpiresCommanderEconomyLevelReset();
void EmpiresObjectiveLevelReset();
void EmpiresReleaseDoorButtonClaim(int userId);
void EmpiresEngibotLevelReset(); // bot_empires_engibot.cpp
bool EmpiresIsEngibot(const edict_t* player);
// TODO 8.2: player_death of any player (bot_events.cpp); remembers teammates killed by the enemy.
void EmpiresNoteFriendlyDeath(edict_t* victim, edict_t* attacker);
// TODO 8.3: a connected human player (not a bot) with this userid / edict (bot_empires_heal.cpp).
bool EmpiresIsHumanUserId(int userId);
bool EmpiresIsHuman(edict_t* player);
// Test only (rcbotd empires_test_near_death): a death of `team` at `at`, killed from `threat`.
void EmpiresInjectNearDeath(int team, const Vector& at, const Vector& threat);
void EmpiresCommanderOrdersLevelReset(); // bot_empires_commander_orders.cpp
bool EmpiresCommanderOrderIsOurs(int team, int client); // bot_empires_commander_orders.cpp

// 3.10b step 3: a vehicle's "I need ammo" call (emp_menu_command 29), one slot per vehicle (up to
// kEmpiresAmmoRequestSlots per team); an engineer walks over and places a supply crate
// (bot_empires_ammo_crate.cpp).
void EmpiresRecordInfantryAmmoCall(edict_t* player);
struct EmpiresAmmoRequest
{
	bool infantry = false;
	int ammoType = -1;
	int reserveBefore = -1;
	int vehicle = -1;
	int driverUserId = -1;
	float until = 0.0f;
	int engineerUserId = -1;
	float claimUntil = 0.0f;
};
constexpr int kEmpiresAmmoRequestSlots = 4;
// The team's call slots (kEmpiresAmmoRequestSlots); a slot is live while vehicle > 0 and curtime <= until.
EmpiresAmmoRequest* EmpiresAmmoRequests(int team);
// The live slot of this vehicle; with create, a free (or the oldest) slot, reset, when it has none.
EmpiresAmmoRequest* EmpiresAmmoRequestForVehicle(int team, int vehicle, bool create);
void EmpiresAmmoRequestClear(int team, int vehicle);

constexpr int kManeuverPassableSlots = 4;

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
	EmpiresVehicleLoadout::Loadout purchaseLoadout;
	EmpiresVehicleLoadout::Loadout refitLoadout;
	float factoryProbeAt = 0.0f; // diagnostic: log the factory state once in WAIT_SPAWN
	std::vector<Vector> factoryRoute; // corners around the factory to its ramp
	std::size_t factoryRouteIndex = 0;
	float nextAction = 0.0f;
	float stageUntil = 0.0f;
	float cooldownUntil = 0.0f;
	float nextEmptyLog = 0.0f;
	float deferredSince = 0.0f; // richer-buyer wait start (5.5a), bounded to 30 s
	Vector walkFrom = Vector(0, 0, 0); // GOTO_FACTORY: position at the last progress check
	int walkRequests = 0;       // GOTO_FACTORY: navigation requests (diagnostic)
	float walkCheckAt = 0.0f;   // next progress check (a walk that still closes in gets more time)
	float walkBestFactory = 0.0f; // GOTO_FACTORY: nearest 2D distance to the factory so far
	float walkBestAt = 0.0f;      // ... and when it was reached (moving-away check over 60 s)
	int walkAwayReplans = 0;      // moving-away escalations in this walk
	float nextDecision = 0.0f;
	int failures = 0;
	int useTries = 0;
	std::vector<int> baseline;
	// Driving state.
	Vector lastVehicleOrigin = Vector(0, 0, 0);
	float lastSampleTime = 0.0f;
	Vector velocity = Vector(0, 0, 0);
	float turnTestStart = 0.0f;
	float turnTestNextSample = 0.0f;
	float turnTestHeading = 0.0f;
	int turnTestMode = 0;
	float slowSince = 0.0f;
	float reverseUntil = 0.0f;
	float reverseSide = 0.0f;
	int stuckReverses = 0;
	bool clearBeforeExit = false; // 3.26 (3b): backed clear once before giving up
	int passengerSeat = -1;       // seated as passenger (APC spawn): sampled vehicle for the speed check
	bool passengerExit = false;   // GIVE_UP_EXIT started as passenger: short cooldown after leaving
	float passengerSince = 0.0f;  // first seen in this passenger seat
	float exitSince = 0.0f;       // GIVE_UP_EXIT start: escalate when the exit stays blocked
	int exitStep = 0;             // 0 = USE only, 1.. = seat change / nudge tried
	float exitNudgeUntil = 0.0f;  // after taking the driver seat: drive a little to free an exit point
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
	std::vector<Vector> routePrecise; // tight-lane approach/entry points (reach closely, no lookahead past)
	float preciseSince = 0.0f;
	std::size_t preciseIndex = static_cast<std::size_t>(-1);
	std::vector<unsigned int> routeAreas;
	std::size_t routeIndex = 0;
	Vector routeGoal = Vector(0, 0, 0);
	float nextReplan = 0.0f;
	float nextAdvanceLog = 0.0f;
	float nextTightLog = 0.0f;          // "vehicle tight" diagnostic beside walls
	// rcbot_empires_vehicle_context_steer
	float ctxNextEval = 0.0f;
	float ctxNextLog = 0.0f;
	float ctxYaw = 0.0f;
	int ctxSlot = -1;                   // -1 route yaw kept, -2 all forward blocked
	float ctxPrevYaw = 0.0f;
	bool ctxHasPrev = false;
	std::size_t ctxNarrowIndex = static_cast<std::size_t>(-1);
	std::size_t ctxNarrowRouteSize = 0;
	Vector ctxNarrowAt = Vector(0, 0, 0);
	bool ctxNarrow = false;
	float ctxAxis = 0.0f;
	float ctxGateFree = 0.0f;
	float nextManeuver = 0.0f;
	float maneuverRecoverUntil = 0.0f;
	float maneuverSteer = 0.0f;
	float maneuverSpeed = 0.0f; // signed, zero means stop
	int maneuverBlocker = -1;
	float nextManeuverBlockLog = 0.0f;
	// Committed detour around an entity on the route: drive beside it, then to an exit point behind it.
	bool detourActive = false;
	int detourStage = 0; // 0 beside the obstacle, 1 exit point
	int detourEntity = -1;
	Vector detourPoint = Vector(0, 0, 0);
	Vector detourExit = Vector(0, 0, 0);
	Vector detourDir = Vector(1, 0, 0);
	float detourUntil = 0.0f;
	// Entities the vehicle overlapped while rolling on: not obstacles for it (expire after 20 s).
	int maneuverPassable[kManeuverPassableSlots] = {-1, -1, -1, -1};
	float maneuverPassableUntil[kManeuverPassableSlots] = {0.0f, 0.0f, 0.0f, 0.0f};
	bool maneuverValid = false;
	// Leaving the factory: straight out of the bay before the first route (human test 15.11.).
	bool exitChecked = false;
	Vector exitPoint = Vector(0, 0, 0);
	float exitUntil = 0.0f;
	bool routeFailed = false;
	bool routePartial = false; // route ends at the reachable area nearest the goal
	int routeResult = -1;
	float width = 0.0f;
	float waterDepth = 0.0f; // deepest water the body centre stays dry in (0 = not measured)
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
	// Net progress of the drive: best distance to cvGoal and best remaining route length since
	// cvProgressAt; neither improving for a while ends the drive (stall-escalate).
	float cvBestGoal = 1.0e9f;
	float cvBestRoute = 1.0e9f;
	float cvProgressAt = 0.0f;
	// Boomtanking: run over enemy infantry inside the base zone, then back to cvGoal.
	bool cvHunt = false;
	Vector cvHome = Vector(0, 0, 0);
	float cvZone = 0.0f;
	int cvKillsTried = 0;
	int cvLastTarget = -1;
	float cvNoPreySince = 0.0f;
	float cvBackUntil = 0.0f; // three-point turn: reversing with opposite lock until then
	float cvBackNext = 0.0f;  // no new three-point turn before then
	Vector cvBackStart = Vector(0, 0, 0);
	// Armed fight: keep moving at range, circling the target, direction changes now and then.
	float fightDir = 0.0f;     // +1 / -1 around the target, 0 = not chosen yet
	float fightSwitch = 0.0f;  // next change of direction
	float fightSight = 0.0f;   // last time the target was in sight
	bool fightMove = false;    // circling this frame
	// 3.26 step 4: persistent fire-blocked escalation (flank -> skip target).
	int fireBlockedTarget = -1;
	float fireBlockedSince = 0.0f;
	int fireBlockedStage = 0;
	float fireBlockedFlankUntil = 0.0f;
	int fireBlockedSkipTarget = -1;
	float fireBlockedSkipUntil = 0.0f;
	// Target motion for leading shots.
	int aimTarget = -1;
	Vector aimLastPos = Vector(0, 0, 0);
	float aimLastTime = 0.0f;
	Vector aimVel = Vector(0, 0, 0);
	float nextAimLog = 0.0f;
	float nextAimCheck = 0.0f;
	// MG engagement on one infantry target (diagnosis 3.21: does the lead hit moving infantry).
	int mgEngTarget = -1;
	int mgLowAimLogged = -1;
	int runOverTarget = -1;      // soldier being run over (wantEmpiresRunOver)
	float runOverUntil = 0.0f;
	float mgEngLastTick = 0.0f;
	float mgEngFired = 0.0f;
	float mgEngMoving = 0.0f;
	float mgEngDistSum = 0.0f;
	int mgEngTicks = 0;
	int mgEngHp0 = 0;
	int mgEngHpLast = 0;
	int cannonEyeLogged = -1;
	float pitchLimitUntil = 0.0f; // 3.21a: cannon pitch clamped (target out of reach) until
	bool pitchLimitDown = false;  // wanted lower than the limit (too close)
	float pitchLimitLogAt = 0.0f;
	int pitchRangeLogged = -1;
	int cannonCacheVehicle = -1;  // vehicle the cached emp_tank_cannon index belongs to
	int cannonCacheIndex = -1;
	float cannonSearchAt = 0.0f;     // 3.21a: vehicle whose cannon pivot was logged
	float mgEngHullOff = 0.0f;   // 3.21a: summed |hull yaw - target yaw| per tick
	float mgEngTurretOff = 0.0f; // summed |turret yaw - target yaw| per tick
	int goalKind = -1; // strategic goal: 0 defend, 1 hunt, 2 front support, 3 push (building), 4 CV
	Vector skippedFront = Vector(0, 0, 0);
	float skipFrontUntil = 0;
	// Push hold point before an enemy building (kind=push): ~1000 HU on the own side, fanned out per bot.
	Vector pushHoldFor = Vector(0, 0, 0);
	Vector pushHold = Vector(0, 0, 0);
	float pushHoldUntil = 0;
	bool pushHoldKnown = false;
	float escortWaitSince = 0.0f;       // TODO 8.4: holding for the escorting engineer
	float escortWaitBlockedUntil = 0.0f;
	float escortRepairHoldSince = 0.0f;
	float nextParkSearch = 0.0f;
	float parkProgressAt = 0.0f;
	float parkBestDistance = 0.0f;
	Vector rejectedPark = Vector(0, 0, 0);
	float rejectedParkUntil = 0.0f;
	float parkArriveAt = 0.0f;    // TODO 5.14a: spawn APC at its spot since (0 = not there)
	float parkArriveTotal = 0.0f; // hull + armor when it got there (damage window of 15 s)
	Vector supportFlag = Vector(0, 0, 0); // front flag the support spot belongs to
	Vector supportSpot = Vector(0, 0, 0);
	bool supportKnown = false;
	Vector goalLogged = Vector(0, 0, 0); // diagnosis only (rcbot_empires_vehicle_shell_trace)
	// Turret loadout of the active fire group, read from the turret's send props (see
	// VehicleWeaponButtons in bot_empires_vehicle.cpp). Masks of IN_ATTACK / IN_ATTACK2.
	float weaponReadAt = -1.0f;
	float loadoutCheckAt = 0.0f;   // 3.10d: periodic check for an MG sharing a button
	float loadoutRetryAt = 0.0f;   // no new loadout trip before this (refit refused or no pad)
	bool loadoutRefit = false;     // the current retreat is for the loadout, not for damage
	bool loadoutVerify = false;    // read and log the loadout once after a loadout refit
	bool ammoTrip = false;         // 3.10b: the loadout trip is for empty cannon/missiles
	bool weaponsKnown = false;
	int weaponMgPure = 0;     // buttons firing only machine guns that have rounds loaded
	int weaponMgMixed = 0;    // buttons with a loaded machine gun that also fire another weapon
	int weaponHeavyInf = 0;   // buttons with a loaded cannon or grenade launcher
	int weaponArmour = 0;     // buttons with a loaded weapon that hurts armour
	bool weaponMgLeft = false; // a machine gun in the group still has magazine or reserve rounds
	bool weaponHeavyPresent = false; // any group has a cannon or missile launcher (3.10b; 3.10f: all groups)
	bool weaponHeavyLeft = false;    // one of them still has magazine or reserve rounds
	bool weaponCannonLeft = false;   // 3.10f: a cannon (any group) has rounds left
	bool weaponMissileLeft = false;  // 3.10f: a missile launcher (any group) has rounds left
	bool weaponActiveCannonLoaded = false;  // active group: a cannon with a loaded magazine
	bool weaponActiveMissileLoaded = false; // active group: a missile launcher with a loaded magazine
	int weaponHeavyTotal = 0;        // magazine + reserve of those weapons, all groups (3.10b step 2)
	int weaponGroup = -1;            // 3.10f: active fire group 0..5 (turret m_iGroup)
	int weaponGroupCount = 0;        // highest group with a weapon + 1
	float fireGroupSentAt = -10.0f;  // last emp_change_vehicweap
	float turretAlignLogAt = 0.0f;   // 3.21a stall-escalate who=vehicle-aim throttle
	int turretAlignTarget = -1;      // target of the last vehicle-aim escalation
	int fireGroupHeld = 0;           // 3.10f: ticks with fire held for a group switch (diagnostic)
	// 3.10b step 2: an ammo trip may end at a supply crate nearer than the pad.
	int crateIndex = -1;             // crate driven to, -1 = pad
	float crateNearSince = -1.0f;    // inside the crate's reach (1000) since
	int crateAmmoSeen = -1;          // highest weaponHeavyTotal seen there
	float crateAmmoRise = 0.0f;      // last time it rose
	float crateReadAt = 0.0f;
	bool crateDone = false;          // refilled at the crate: end the trip
	int crateSkip = -1;              // crate that gave nothing for 30 s: use the pad
	float nextAmmoCall = 0.0f;       // 3.10b step 3: next "I need ammo" call
	char weaponSummary[96] = {};
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
	Vector padProgressOrigin = Vector(0, 0, 0);
	float padProgressSince = -1.0f;
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
	bool requestBootstrapRemoval();
	bool removalRequested() const { return m_bRemovalRequested; }
	bool isBootstrapOnly() const override { return m_bBootstrapOnly; }
	bool isControlledTestOnly() const override { return m_bBootstrapOnly; }
	bool isEnemy(edict_t* pEdict, bool bCheckWeapons = true) override;
	bool wantToListenToPlayerAttack(edict_t* pPlayer, int iWeaponID = -1) override;
	float getEnemyFactor(edict_t* pEnemy) override;
	void enemyFound(edict_t* pEnemy) override;
	void receiveEmpiresSpottedEnemy(edict_t* pEnemy, int sourceIndex);
	void updateConditions() override;
	edict_t* getCurrentControlledTestEdict() const;
	void setup() override;
	bool suppressBotThink() override;
	bool startGame() override;
	bool hurt(edict_t* pAttacker, int iHealthNow, bool bDontHide = false) override;
	void died(edict_t* pKiller, const char* pszWeapon) override;
	void killed(edict_t* pVictim, char* weapon) override;
	void modThink() override;
	void getTasks(unsigned iIgnore = 0) override;
	void handleWeapons() override;
	void getLookAtVector() override;
	bool handleCustomLookAtTask(eLookTask lookTask, float fTime) override;
	bool applyCustomLookAngles(const QAngle& requiredAngles, QAngle& viewAngles) override;
	void freeMapMemory() override;
	bool requestNormalPathOnce(int startWaypoint = -1,
		int destinationWaypoint = -1, bool terminalAfterStop = false);
	bool requestEmpiresVehicleFactoryDriveOnce();
	// Vehicle use (bot_empires_vehicle.cpp).
	void tickEmpiresVehicle();
	void tickEmpiresVoidSuicide();
	// Team director (bot_empires_team_director.cpp, TODO 5.4/5.8/5.28): team blackboard and kill-spawn.
	void tickTeamDirector();
	void tickEmpiresMatchFlow(); // bot_empires_match_flow.cpp, MATCH_FLOW.md
	void tickEmpiresKillSpawn();
	bool findNearestEmpiresSpawn(int team, const Vector& to, int& spawnId, Vector& at) const;
	float m_flKillSpawnNextCheck = 0.0f;
	float m_flKillSpawnLast = -1.0e9f;
	float m_flKillSpawnNextBlockLog = 0.0f;
	mutable int m_iKillSpawnId = -1;          // spawn chosen before the kill, used by the next spawn choice
	mutable float m_flKillSpawnIdUntil = 0.0f;
	mutable bool m_bKillSpawnMissed = false;  // set by the spawn choice when the pre-selected spawn was unusable
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
	bool handleEmpiresWreckDeconstruct();
	bool handleEmpiresFactoryBayEnemy();
	// True while this engineer holds the team's vehicle repair call (claim not expired).
	bool empiresVehicleRepairClaimed() const;
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
	friend bool EmpiresBotWantsVfSpawn(const CBotEmpires*, int);
	friend struct EmpiresLawlietImpl;
	void tickEmpiresLawliet();
	bool handleEmpiresLawlietTask();
	bool lawlietBlocksWeapons() const;
	bool applyEmpiresLawlietCommand(CBotCmd& command);
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
	bool m_bCommanderSiteDrive = false; // stages 3/4: drive the CV to a remote factory site, then back to the commander view
	float m_fCommanderHuntZone = 0.0f;
	// Automatic Commander (bot_empires_commander_economy.cpp, cvar rcbot_empires_auto_commander).
	void tickCommanderEconomy();
	void tickCommanderOrders(int team, float now, const Vector& home); // bot_empires_commander_orders.cpp
	void releaseCommanderOrders(int team, const char* reason); // bot_empires_commander_orders.cpp
	static CBotEmpires* commanderSeatCandidate(int team);
	// Seat candidate, approaching, seated or out building: no refinery turrets.
	bool commanderDutyBlocksSideTasks();
	// Test-only: act as if a human of the team wanted the Commander seat.
	static void requestCommanderYieldTest(int team);
	bool isEmpiresEngineerClass() const;
	// Human commander: up to rcbot_empires_engineer_help_human free engineers of the team build or
	// repair unfinished/damaged own buildings near the CV (the bot commander's economy does this
	// otherwise). Called every 10 s from EmpiresEngibotFrame.
	static void empiresHelpHumanCommander();
	struct CommanderEconomyState
	{
		float nextTick = 0.0f;
		float nextEntryTry = 0.0f;
		int entryAttempts = 0;
		int lastSelfBuildSkip = 0;
		float nextTurretCheck = 0.0f;
		float nextHuntCheck = 0.0f;
		float nextRetreatCheck = 0.0f;
		float lastCvHealth = -1.0f;
		float cvMaxHealth = 0.0f;
		float blockedUntil[EmpiresCommanderEconomy::TypeCount] = {};
		int failures[EmpiresCommanderEconomy::TypeCount] = {};
		bool verifyPending = false;
		int verifyType = 0;
		int verifyResIndex = -1;
		float verifyAt = 0.0f;
		float verifyResources = 0.0f;
		std::vector<int> verifyBefore;
		std::vector<std::pair<int, float>> refineryPointBlocked; // res index, until
		struct AssignTry { int entity = 0; int tries = 0; float next = 0.0f; int rounds = 0; int lastUserId = -1; };
		std::vector<AssignTry> assignTries;
		int lastLogKey = -1;
		float escortUntil = 0.0f; // an Engineer walks toward a resource point out of reach
		float nextResearch = 0.0f;
		float nextResearchLog = 0.0f;
		int lastResearchActive = -1;
		bool lastResearchPaused = false;
		bool hadResearchBuilding = false;
		// Remote factory site (TODO 5.8c): free 1200x1200 spot found by a ground scan when the base has none.
		bool siteValid = false;
		Vector site;
		float siteRetryAt = 0.0f;
		float forwardBarracksRetryAt = 0.0f; // forward barracks beside a far factory spot (user 20.11.)
		float forwardPadRetryAt = 0.0f; // forward repair pad beside a far finished barracks/factory (TODO 5.5)
		Vector siteScanOrigin = Vector(0, 0, 0); // CV position at the first factory search: the scan stays anchored at the base
		bool siteScanOriginSet = false;
		bool nearSiteTried = false; // base factory spot reserved before the first base building (user 20.11.)
		std::vector<Vector> siteExcluded;
	} m_commanderEconomy;
	// An Engineer walks to one assigned building and works on it until full.
	// tier: EmpiresPriority tier of the order, -1 = from the building class (bot_empires_priority.h).
	bool requestEmpiresBuildAssignment(edict_t* building, int tier = -1);
	EmpiresPriority::Job empiresEngineerJob() const;
	bool tickEmpiresEngibot(); // bot_empires_engibot.cpp
	bool empiresRepairCallAllowed(edict_t* hurt); // bot_empires_vehicle.cpp
	bool commanderOrderHoldsFrom(edict_t* vehicle); // bot_empires.cpp
	bool tickEngibotTurretOrder(struct EngibotTurretJob& job, int team, int order); // bot_empires_engibot.cpp
	void applyEmpiresTeamClassName(); // bot_empires_engibot.cpp
	void tickEmpiresBuildAssignment();
	void tickEmpiresGuardOrder();
	float m_fEmpiresGuardNextCheck = 0.0f;
	int m_iEmpiresGuardLoggedTarget = -1;
	int m_iEmpiresTestGuardTarget = 0; // test-only: entindex acting as Guard target
	void queueEmpiresBuildAssignRoute();
	int m_iBuildAssignStage = 0;
	int m_iBuildAssignTier = EmpiresPriority::TIER_IDLE;
	int m_iBuildAssignAttempts = 0;
	MyEHandle m_hBuildAssignBuilding;
	float m_fBuildAssignStart = 0.0f;
	float m_fBuildAssignNextTry = 0.0f;
	Vector m_vBuildAssignSpot = Vector(0.0f, 0.0f, 0.0f);
	// Progress (TODO 3.26): health gain or movement resets m_fBuildAssignProgressAt; 20 s without
	// either picks another stand spot (bad spots excluded), the third stall gives up.
	float m_fBuildAssignProgressAt = 0.0f;
	int m_iBuildAssignBestHealth = -1;
	Vector m_vBuildAssignProgressPos = Vector(0.0f, 0.0f, 0.0f);
	int m_iBuildAssignEscalations = 0;
	bool m_bBuildAssignRequireSpot = false;
	std::vector<Vector> m_vBuildAssignBadSpots;
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
		bool requireFullHealth = false,
		int buildItem = 1);
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
	// Infantry side-steps, keeps its weapon band and backs off in a fight (bot_empires_skirmish.cpp).
	bool handleEmpiresSkirmish();
	void endEmpiresSkirmish(const char* reason);
	bool empiresSkirmishSpotSafe(const Vector& origin, const Vector& want, edict_t* enemy, Vector& out) const;
	bool m_bSkirmishActive = false;
	MyEHandle m_pSkirmishEnemy;
	Vector m_vSkirmishGoal;
	float m_flSkirmishStarted = 0.0f;
	float m_flSkirmishLastSeen = 0.0f;
	float m_flSkirmishPhaseStarted = 0.0f;
	float m_flSkirmishPhaseUntil = 0.0f;
	float m_flSkirmishDisabledUntil = 0.0f;
	int m_iSkirmishPhase = 0; // 0 none, 1 step, 2 stand/crouch, 3 step reached
	int m_iSkirmishSide = 1;
	int m_iSkirmishSteps = 0;
	int m_iSkirmishBlocks = 0;
	int m_iSkirmishPauses = 0;
	int m_iSkirmishStage = 0;
	int m_iSkirmishCovers = 0;
	float m_flSkirmishTick = 0.0f;
	bool m_bSkirmishNoStepLogged = false;
	mutable int m_iSkirmishRejectMask = 0;
	mutable char m_szSkirmishRejectHit[48] = {};
	int m_iSkirmishOddsBack = 0;
	int m_iSkirmishAllies = 0;
	int m_iSkirmishEnemies = 0;
	float m_flSkirmishOddsAt = 0.0f;
	int m_iSkirmishClassMode = 0; // 0 normal, 1 scout flank, 2 grenadier vs vehicle, 3 engineer rear
	int m_iSkirmishFlanks = 0;
	int m_iSkirmishRearHolds = 0;
	bool findEmpiresFrontAlly(edict_t* enemy, const Vector& enemyPos, Vector& allyPos, float& allyDist) const;
	bool empiresHasRocket() const;
	// Diagnosis: an RPG shot at a vehicle and whether its hull+armor dropped 3 s later.
	void trackEmpiresRocketShot(float now);
	int m_iRpgLastCount = -1;
	int m_iRpgShotVehicle = 0;
	float m_flRpgShotCheck = 0.0f;
	float m_flRpgShotBefore = 0.0f;
	float m_flRpgShotDist = 0.0f;
	// Diagnosis for the calm idle look: picks, picks toward the last enemy, largest level turn.
	int m_iCalmLookPicks = 0;
	int m_iCalmLookEnemy = 0;
	float m_flCalmLookMaxTurn = 0.0f;
	float m_flCalmLookLogAt = 0.0f;
	// Infantry realistic look & combat polish (bot_empires_look.cpp).
	static constexpr int kEmpiresMaxTrackedWeapons = 64;
	void resetEmpiresLookState();
	bool empiresInfantryLookContext() const;
	bool resolveEmpiresRealisticLookVector();
	void tickEmpiresTacticalReload();
	bool empiresCombatFireReady(edict_t* pEnemy, CBotWeapon* pWeapon);
	EmpiresLookLogic::SpringAxis m_lookPitchSpring;
	EmpiresLookLogic::SpringAxis m_lookYawSpring;
	EmpiresLookLogic::HeadSteadyTracker m_lookHeadSteady;
	float m_flLookSpringLastTime = 0.0f;
	EmpiresLookLogic::LookPriority m_eLookContentPriority = EmpiresLookLogic::LookPriority::BORING;
	float m_flLookContentExpireAt = 0.0f;
	float m_flVerticalLookHoldUntil = 0.0f;
	float m_flNavLookLogAt = 0.0f;
	float m_flSmartIdleLogAt = 0.0f;
	int m_iSmartIdleWallPicks = 0;
	int m_iSmartIdleOpenPicks = 0;
	float m_flCombatOffTargetSince = -1.0f;
	float m_flCombatBurstStart = -1.0f;
	float m_flCombatPolishLogAt = 0.0f;
	float m_flTacticalReloadNextCheck = 0.0f;
	int m_iMaxClipSeen[kEmpiresMaxTrackedWeapons] = {};
	bool handleEmpiresScoutBinoculars();
	float m_flNextBinocularSpot = 0.0f;
	void countEmpiresFightOdds(const Vector& origin, int& allies, int& enemies);
	bool findEmpiresSkirmishCover(const Vector& origin, edict_t* enemy, const Vector& toEnemy, Vector& out) const;
	void calloutEmpiresEnemy(edict_t* enemy);
	float m_flNextCallout = 0.0f;
	void postEmpiresContact(const Vector& enemyPos);
	bool handleEmpiresJoinFight();
	void endEmpiresJoinFight(const char* reason);
	// TODO 8.2: teammates fell next to me -> cover / short fall-back (bot_empires_skirmish.cpp).
	bool empiresShaken(float now);
	bool handleEmpiresShaken();
	bool empiresDeployBusy() const; // wall/radar job at work or < 800 from its stand
	// TODO 8.3: damaged standing own vehicle of a human (or parked empty) near this engineer.
	edict_t* findEmpiresBuddyRepairVehicle(int& worstSide, bool& armorDamaged, int& driverUserId);
	float m_flBuddyRepairScan = 0.0f;
	// TODO 8.4: follow an own tank and repair it at the front (bot_empires_vehicle.cpp).
	bool handleEmpiresTankEscort();
	void endEmpiresTankEscort(const char* reason);
	int m_iEscortVehicle = -1;
	float m_flEscortNextScan = 0.0f;
	float m_flEscortNextNav = 0.0f;
	float m_flEscortProgressAt = 0.0f;
	float m_flEscortBestDist = 1.0e9f;
	float m_flEscortCooldownUntil = 0.0f;
	int m_iEscortRejected = -1;
	Vector m_vEscortNavGoal = Vector(0, 0, 0);
	Vector m_vEscortTankLast = Vector(0, 0, 0);
	Vector m_vEscortBehind = Vector(0, 0, 0);
	void endEmpiresShaken(const char* reason);
	void takeEmpiresMovement(const char* why);
	float m_flShakenUntil = 0.0f;
	float m_flShakenStarted = 0.0f;
	float m_flShakenNext = 0.0f;      // cooldown
	float m_flShakenAfter = -1.0e9f;  // deaths up to this time are used up
	float m_flShakenGoalSet = 0.0f;
	Vector m_vShakenThreat;
	Vector m_vShakenGoal;
	Vector m_vShakenStart;
	int m_iShakenStage = 0;           // 0 moving, 1 second spot, 2 crouch in place
	bool m_bShakenActive = false;     // the fall-back movement owns the bot
	bool m_bShakenHasGoal = false;
	bool pickEmpiresShakenSpot(const Vector& origin, bool avoidCurrent, Vector& out) const;
	// Map change / new bot: gpGlobals->curtime restarts at 0, so stored times would block or misfire.
	void resetEmpiresSkirmishState(bool mapChange);
	float m_flNextContactPost = 0.0f;
	int m_iJoinContactId = 0;
	int m_iJoinSkipId = 0;
	Vector m_vJoinGoal;
	float m_flJoinStarted = 0.0f;
	float m_flJoinProgressAt = 0.0f;
	float m_flJoinBestDist = 0.0f;
	float m_flJoinNextNav = 0.0f;
	float m_flJoinNextSearch = 0.0f;
	// Engineer heals hurt teammates nearby with the kit (bot_empires_heal.cpp).
	bool handleEmpiresHeal();
	void endEmpiresHeal(const char* reason, float skipFor);
	void tickEmpiresHealPatient();
	bool empiresHealPatientWaiting();
	bool m_bHealPatientHeld = false;
	void holdEmpiresHealPosition();
	void lookEmpiresHealTarget(const Vector& at);
	float m_flHealNextLog = 0.0f;
	int m_iHealTarget = 0;
	float m_flHealHumanCheck = 0.0f;
	int m_iHealTargetUserId = -1;
	int m_iHealStartHealth = 0;
	int m_iHealLastHealth = 0;
	int m_iHealSkipUserId = -1;
	float m_flHealSkipUntil = 0.0f;
	float m_flHealNextSearch = 0.0f;
	float m_flHealStarted = 0.0f;
	float m_flHealProgressAt = 0.0f;
	float m_flHealBestDistance = 0.0f;
	float m_flHealNextNav = 0.0f;
	bool m_bHealAttacking = false;
	bool m_bHealInReach = false;
	Vector m_vHealNavGoal;
	// 5.8a / 3.17: Engineer carcass deconstruction (VF bay & base wrecks) + Grenadier bay enemy clear.
	int m_iWreckClearTarget = 0;
	int m_iWreckClearLastHp = -1;
	bool m_bWreckClearBay = false;
	bool m_bWreckClearAttacking = false;
	bool m_bWreckClearProgressLogged = false;
	float m_flWreckClearNextSearch = 0.0f;
	float m_flWreckClearNextNav = 0.0f;
	float m_flWreckClearProgressAt = 0.0f;
	float m_flWreckClearBestDist = 1.0e9f;
	float m_flBayEnemyNextNav = 0.0f;
	int m_iReviveTarget = 0;
	int m_iReviveTargetUserId = -1;
	struct ReviveRetry { int targetUserId; float until; };
	std::vector<ReviveRetry> m_reviveRetries;
	float m_flReviveUntil = 0.0f;
	float m_flReviveBestDistance = 0.0f;
	bool m_bReviveReplanned = false;
	float m_flReviveNextNav = 0.0f;
	float m_flReviveSent = 0.0f;
	float m_flReviveNextSearch = 0.0f;
	float m_flReviveNextLog = 0.0f;
	float m_flReviveDeathTime = 0.0f;
	bool m_bReviveHoldLogged = false;
	bool m_bReviveHoldActive = false;
	void tickEmpiresReviveGetUp();
	float m_flReviveGetUpSince = -1.0f;
	float m_flReviveGetUpNextPress = 0.0f;
	float m_flReviveGetUpStoodAt = -1.0f;
	float m_flReviveGetUpHeight0 = -1.0f;
	int m_iReviveGetUpPresses = 0;
	bool empiresSniperReady(edict_t* pEnemy, CBotWeapon* pWeapon);
	bool wantEmpiresRunOver(EmpiresVehicleBrain& v, edict_t* target, const Vector& targetAt, const Vector& origin,
		float now, int userId);
	void relaxEmpiresSniperScope();
	int m_iSniperTarget = -1;
	int m_iSniperShots = 0;
	int m_iSniperToggleFails = 0;
	bool m_bSniperToggleExpect = false;
	float m_flSniperSettleUntil = 0.0f;
	float m_flSniperNextShot = 0.0f;
	float m_flSniperToggleAt = 0.0f;
	float m_flSniperLastAim = 0.0f;
	float m_flSniperLogAt = 0.0f;
	bool holdRespawnForRevive();
	// Engineer builds an MG turret on the front side of own refineries (bot_empires_turret_build.cpp).
	bool handleEmpiresRefineryTurret();
	// Engineer answers a vehicle's ammo call with a supply crate (bot_empires_ammo_crate.cpp).
	bool handleEmpiresAmmoRequest();
	int m_iAmmoCrateStage = 0;      // 0 idle, 1 walk, 2 select, 3 aim, 4 observe
	int m_iAmmoCrateVehicle = -1;
	int m_iAmmoCrateTries = 0;
	int m_iAmmoCrateBaseline = 0;   // crates near the engineer before the attempt
	std::vector<int> m_ammoCrateBefore; // own crates near him then (index + serial * 4096)
	float m_flAmmoCrateStageAt = 0.0f;
	float m_flAmmoCrateNextNav = 0.0f;
	float m_flAmmoCrateNextSearch = 0.0f;
	float m_flAmmoCrateBestDist = 0.0f;   // nearest the walk got (progress check)
	float m_flAmmoCrateProgressAt = 0.0f;
	Vector m_vAmmoCrateTarget;
	Vector m_vAmmoCrateStand; // after a miss: a new spot beside the vehicle
	bool m_bAmmoCrateMove = false;
	int m_iTurretRefinery = 0;
	int m_iTurretAttempt = 0;
	int m_iTurretBuilding = 0;
	int m_iTurretBuildItem = 1; // 1 = MG turret, 2 = ML turret
	int m_iTurretAssignRejects = 0;
	bool m_bEmpiresRepairRole = false; // skill plan: Repair first (builds turrets); Revive role stays with the team
	bool m_bTurretBuildArmed = false;
	Vector m_vTurretSpot;
	float m_flTurretYaw = 0.0f;
	float m_flTurretUntil = 0.0f;
	float m_flTurretNextNav = 0.0f;
	float m_flTurretNextSearch = 0.0f;
	float m_flTurretNextLog = 0.0f;
	// Turret upgrade (5.6, emp_eng_upgrade): own fully built turret, needs skill gate +0x121e.
	int m_iTurretUpgrade = 0;
	int m_iTurretRestrictSeen = -2;
	int m_iTurretUpgradeLevel = 0;
	int m_iTurretUpgradeSends = 0;
	float m_flTurretUpgradeNextSearch = 0.0f;
	float m_flTurretUpgradeUntil = 0.0f;
	float m_flTurretUpgradeAimSince = 0.0f;
	float m_flTurretUpgradeNextSend = 0.0f;
	bool handleEmpiresTurretUpgrade();
	// Engineer deployables (bot_empires_deployables.cpp): camera/radar (kit items 4/5) at own refineries,
	// a three-part wall (item 6) on a refinery's enemy side.
	bool handleEmpiresEngineerDeployables();
	struct EmpiresDeployJob
	{
		int stage = 0;          // 0 idle, 1 walk, 2 select, 3 aim, 4 observe, 5 build up
		int item = 0;           // 4 camera, 5 radar, 6 walls
		int refinery = 0;
		int segment = 0;        // walls: next point 0..3 of the chained line
		int click = 0;          // walls: new pieces so far
		int tries = 0;
		int built = 0;          // entity being built up
		Vector stand;
		Vector aim[2];
		std::vector<int> before;
		std::vector<int> skip;  // wall pieces whose build-up timed out
		std::vector<std::pair<int, int>> pieceSeg; // wall piece key -> segment (1..3) it was placed with
		float stageAt = 0.0f;
		float until = 0.0f;
		float nextNav = 0.0f;
		float nextSearch = 0.0f;
		float bestDist = 0.0f;
		float radius = 0.0f;    // walls: middle piece distance from the refinery
		float progressAt = 0.0f;
		float radarBlockedUntil = 0.0f;
		float nextLog = 0.0f;
		float lastTick = 0.0f;
		bool resetChain = false; // walls: clear the kit point counter before the first point
		bool sideFlip = false;
		bool aimAlt = false;     // build-up: aim at the piece box center instead of the segment middle
		int lastHealth = -1;
		int progressHealth = -1;
		int progressPiece = -1;
		float buildAt = 0.0f;
		float reachAt = 0.0f;
		float healthAt = 0.0f;   // build-up: work from the far side of the piece
		bool firstPoint = false; // walls: the next click starts a chain (creates no piece)
	} m_deploy;
	bool handleEmpiresGrenadierTurretAttack();
	bool handleEmpiresBuildingAttack();
	// A grenadier with rockets breaks off any task, commander orders included, for an RPG shot at an
	// enemy vehicle in range and sight (bot_empires_objective.cpp).
	bool handleEmpiresRocketVehicle();
	void armEmpiresRocket(edict_t* target, edict_t* launcher, int ammoType, const QAngle& aim, int health,
		const char* rpgName);
	bool m_bEmpiresRocketVehicleActive = false;
	float m_flEmpiresRocketVehicleNext = 0.0f;
	void abortEmpiresBuildingAction(const char* reason);
	bool armEmpiresSeismicThrow(float now, int userId, int team);
	void tickEmpiresSeismicTrack();
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
	int m_iNormalSpawnExcludeId = -1;
	int m_iNormalSpawnRetryCount = 0;
	float m_flNormalSpawnRetryAfter = 0.0f;
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
	float m_flMeshCoverUntil = 0;
	bool m_bEmpiresCoverScheduleActive = false;
	float m_flCoverHoldLogAt = 0.0f; // throttle for the cover holding-fire diagnostic
	EmpiresSkirmishLogic::HurtWindow m_hurtWindow; // 3.25b hurt-cover trigger (35 % HP in 2 s, cooldown)
	float m_flHurtSkipLogAt = 0.0f;
	void logEmpiresHurtCoverSkip(const char* reason, int healthNow);
	void pollEmpiresHurt(int health, bool worldDamage, const Vector* damageOrigin);
	// 3.25b health polling (no player_hurt in Empires): set while modThink feeds a drop into hurt().
	float m_flIntelReportAt = 0.0f; // 3.20 throttle for EmpiresTeamIntelReportSight
	float m_flMortarHiddenLogAt = 0.0f; // 3.20 throttle for building-mortar skip reason=not-known
	bool m_bPolledHurt = false;
	bool m_bPolledHurtInterruptEngineer = false;
	int m_iPolledHurtDamage = 0;
	bool m_bPolledHurtOriginValid = false;
	Vector m_vPolledHurtOrigin;
	int m_iPolledHurtHits = 0;      // diagnostic since the last hurt-poll line
	int m_iPolledHurtDamageSum = 0;
	float m_flPolledHurtLogAt = 0.0f;
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
	// Last enemy memory came from a teammate report, not from this bot's own sight.
	bool m_bEmpiresLastEnemyReported = false;
	float m_flHurtLookLogAt = 0.0f;
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
	int m_iEmpiresEngineerBuildActionItem = 1; // 1 = MG turret, 2 = ML turret
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
	// Commander order type that started the route (1 move, 2 attack location, 3 attack target,
	// 4 guard; reverse-engineering/notes/commander-order-types.md) and its target entity (3/4).
	int m_iEmpiresCommanderOrderType = 0;
	mutable int m_iEmpiresCommanderOrderTargetEnt = 0; // written by the const reader
	float m_fEmpiresCommanderOrderRearmAt = 0.0f;
	int m_iEmpiresCommanderOrderSeenType = -1;
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
	bool m_bEmpiresTurretRocketIsThreat = false;
	float m_flEmpiresBuildingAttackNext = 0.0f;
	MyEHandle m_hEmpiresBuildingActionTarget;
	int m_iEmpiresBuildingActionMode = 0; // 0 none, 1 sticky/stun throw, 2 scout sabotage, 3 scout stun approach
	float m_flEmpiresBuildingActionUntil = 0.0f;
	float m_flEmpiresBuildingActionReadyAt = 0.0f;
	float m_flEmpiresBuildingActionPressUntil = 0.0f;
	bool m_bEmpiresBuildingActionThrown = false;
	// Engineer seismic throw (5.31.2): mode 1 with a ballistic pitch, plus a flight/damage trace.
	bool m_bEmpiresBuildingActionSeismic = false;
	bool m_bEmpiresBuildingActionSeismicRunning = false;
	float m_flEmpiresSeismicPitch = 0.0f;
	float m_flEmpiresSeismicHoldUntil = 0.0f;
	int m_iSeismicBurstTarget = 0;
	int m_iSeismicBurstHealth = 0;
	int m_iSeismicBurstThrows = 0;
	int m_iSeismicSkipTarget = 0;
	float m_flSeismicSkipUntil = 0.0f;
	int m_iSeismicTrackProjectile = 0;
	int m_iSeismicTrackTarget = 0;
	int m_iSeismicTrackHealth = 0;
	float m_flSeismicTrackStart = 0.0f;
	float m_flSeismicTrackLastSeen = 0.0f;
	float m_flSeismicTrackResultAt = 0.0f;
	float m_flSeismicNoneLog = 0.0f;
	Vector m_vSeismicTrackLast = Vector(0, 0, 0);
	Vector m_vSeismicTrackFirst = Vector(0, 0, 0);
	Vector m_vSeismicTrackTargetAt = Vector(0, 0, 0);

	// Pressing a map button that opens a closed door (as_escort base gate control1).
	int m_iEmpiresDoorButton = -1;
	int m_iEmpiresDoorButtonDoor = -1;
	Vector m_vEmpiresDoorButtonStand = Vector(0, 0, 0);
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

edict_t* EmpiresCarriedWeapon(const CBaseHandle& handle);
bool EmpiresVehicleExceedsSpeed(edict_t* vehicle, float limit);
bool EmpiresReadTeamResearch(int team, bool done[EmpiresCommanderEconomy::kResearchItemCount]);

// 3.20 fair team intel (bot_empires_team_director.cpp). With rcbot_empires_fair_intel off for the
// team: Enabled false, Knows true, Board nullptr (consumers keep the old full-entity view).
bool EmpiresFairIntelEnabled(int team);
const EmpiresTeamIntel::Board* EmpiresTeamIntelBoard(int team);
float EmpiresTeamDirectorVehicleGraceLeft(int team); // rcbot_empires_vehicle_vf_grace, bot_empires_team_director.cpp
bool EmpiresTeamIntelKnows(int team, edict_t* enemy);
bool EmpiresTeamIntelKnownPos(int team, edict_t* enemy, Vector& at);
class CBotEmpires;
void EmpiresTeamIntelReportSight(const CBotEmpires* reporter, edict_t* enemy);
bool EmpiresTeamIntelEnemyBase(int team, Vector& at, bool& live);
// rcbot_empires_threat_map (bot_empires_team_director.cpp, bot_empires_threat_map.h). Threat < 0 = map off
// or not built for the team (consumers keep their old behaviour).
float EmpiresThreatAt(int team, const Vector& at, bool vehicleOnly = false);
// Engineer stalled at `at` against a known enemy vehicle on the way to / at `building` (TODO 9.8).
void EmpiresThreatMarkVehicleBlock(int team, int building, const Vector& at);
// True while no engineer should be sent to `building` (own or nearby vehicle-block mark active).
bool EmpiresThreatBuildingBlocked(int team, edict_t* building);

#endif // _BOT_EMPIRES_H_
