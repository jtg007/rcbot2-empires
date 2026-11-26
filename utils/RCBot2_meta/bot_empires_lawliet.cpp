// Lawliet mode runtime (TODO 5.14b), see bot_empires_lawliet.h.
// Team coordinator runs every ~0.5 s from CBotEmpires::tickEmpiresLawliet().
// Crew foot tasks run from CBotEmpires::handleEmpiresLawlietTask() (called by getTasks()),
// and direct UserCmd overrides (boarding IN_USE, grenade throws, ammo crate placement &
// crate resupply) are emitted cleanly at the RunPlayerMove boundary via applyEmpiresLawlietCommand().
#include "bot.h"
#include "bot_buttons.h"
#include "bot_cvars.h"
#include "bot_empires.h"
#include "bot_empires_binary.h"
#include "bot_empires_lawliet.h"
#include "bot_empires_team_director.h"
#include "bot_getprop.h"
#include "bot_globals.h"
#include "bot_mods.h"
#include "bot_navmesh_api.h"
#include "bot_schedule.h"
#include "bot_weapons.h"
#include "in_buttons.h"
#include "iplayerinfo.h"
#include "server_class.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

extern IVEngineServer* engine;
extern IPlayerInfoManager* playerinfomanager;
extern IServerPluginHelpers* helpers;
extern ICvar* g_pCVar;

int EmpiresVehicleReadInt(const CBotEmpires* bot, const edict_t* e, const char* prop, int fallback);
int EmpiresVehiclePlayerVehicle(const CBotEmpires* bot, const edict_t* player);
bool EmpiresVehicleAlive(const CBotEmpires* bot, const edict_t* vehicle);

namespace
{
using namespace EmpiresLawliet;

constexpr float kCoordInterval = 0.5f;
constexpr float kMaxMove = 368.0f;
constexpr int kChassisSeats[14] = {4, 8, 2, 2, 1, 2, 0, 0, 2, 8, 4, 2, 1, 2};

enum BotActionMode
{
	ACT_NONE = 0,
	ACT_BOARD_ENTER,
	ACT_THROW_GRENADE,
	ACT_PLACE_CRATE,
	ACT_RESUPPLY_CRATE
};

struct TeamLawlietState
{
	int phase = PHASE_IDLE;
	float nextCoord = 0.0f;
	float matchStart = -1.0f;
	float phaseSince = 0.0f;
	float cooldownUntil = 0.0f;
	float nextSkipLog = 0.0f;

	int crewClients[kMaxCrew] = {};
	int crewRoles[kMaxCrew] = {};
	int crewCount = 0;
	int driverClient = -1;

	int apcEntity = -1;
	int apcSerial = -1;
	Vector rallyPoint = Vector(0, 0, 0);
	bool rallyValid = false;

	int enemyCvEntity = -1;
	Vector enemyCvOrigin = Vector(0, 0, 0);
	bool enemyCvAlive = false;

	float firstPassengerAt = -1.0f;
	float crewProgressAt = 0.0f;
	float crewBestDist[kMaxCrew] = {};
	bool scoutReassignTried = false;

	float bestCvDist = 1.0e9f;
	float rushProgressAt = 0.0f;
	Vector rushAnchor = Vector(0, 0, 0);

	bool cvStunned = false;
	bool cvStunnedEver = false;
	float cvStunUntil = 0.0f;
	float lastStunThrowAt = -1.0e9f;
	float lastStunThrowDist = 1.0e9f;

	int crateEntity = -1;
	Vector crateOrigin = Vector(0, 0, 0);
};

struct BotLawlietAction
{
	int userId = -1;
	int mode = ACT_NONE;
	int subStage = 0;
	float stageAt = 0.0f;
	float nextAction = 0.0f;
	float nextNav = 0.0f;
	float cooldownUntil = 0.0f;
	Vector navGoal = Vector(0, 0, 0);

	// Grenade throw
	bool isStunThrow = false;
	char weaponName[64] = {};
	int ammoType = -1;
	int ammoBefore = -1;
	float pressUntil = 0.0f;
	float readyAt = 0.0f;
	float observeUntil = 0.0f;
	int projBefore = -1;
	bool thrown = false;

	// Ammo crate placement
	int crateTries = 0;
	int crateBaseline = 0;
	bool crateMove = false;
	Vector crateTarget = Vector(0, 0, 0);
	Vector crateStand = Vector(0, 0, 0);

	// Outgoing command override (only active during interactive actions).
	bool cmdOverride = false;
	float cmdForward = 0.0f;
	float cmdSide = 0.0f;
	int cmdButtons = 0;
	QAngle cmdView = QAngle(0, 0, 0);
};

TeamLawlietState s_team[4];
BotLawlietAction s_bot[kMaxClients];

float Dist2D(const Vector& a, const Vector& b)
{
	return (a - b).Length2D();
}

bool IsClass(const edict_t* e, const char* name)
{
	return e != nullptr && !e->IsFree() && e->GetClassName() != nullptr &&
		std::strcmp(e->GetClassName(), name) == 0;
}

bool IsApcChassis(int chassis)
{
	return chassis == 1 || chassis == 9;
}

bool IsCommanderChassis(int chassis)
{
	return chassis >= 14 && chassis <= 16;
}

bool ContainsCaseInsensitive(const char* haystack, const char* needle)
{
	if (haystack == nullptr || needle == nullptr || *needle == '\0')
		return false;
	const size_t nlen = std::strlen(needle);
	for (const char* p = haystack; *p != '\0'; ++p)
	{
		size_t k = 0;
		while (k < nlen && p[k] != '\0' &&
			std::tolower(static_cast<unsigned char>(p[k])) == std::tolower(static_cast<unsigned char>(needle[k])))
			++k;
		if (k == nlen)
			return true;
	}
	return false;
}

Vector EntityCenter(edict_t* e)
{
	const Vector origin = CBotGlobals::entityOrigin(e);
	if (e != nullptr && e->GetCollideable() != nullptr)
	{
		Vector mins, maxs;
		e->GetCollideable()->WorldSpaceSurroundingBounds(&mins, &maxs);
		if (maxs.z > mins.z)
			return (mins + maxs) * 0.5f;
	}
	return origin + Vector(0.0f, 0.0f, 48.0f);
}

Vector NearestOnEntity(edict_t* e, const Vector& p)
{
	if (e == nullptr)
		return p;
	ICollideable* c = e->GetCollideable();
	if (c == nullptr)
		return CBotGlobals::entityOrigin(e);
	matrix3x4_t toWorld;
	AngleMatrix(c->GetCollisionAngles(), c->GetCollisionOrigin(), toWorld);
	Vector local;
	VectorITransform(p, toWorld, local);
	const Vector mins = c->OBBMins();
	const Vector maxs = c->OBBMaxs();
	for (int i = 0; i < 3; ++i)
		local[i] = (std::max)(mins[i], (std::min)(maxs[i], local[i]));
	Vector out;
	VectorTransform(local, toWorld, out);
	return out;
}

bool ReadEntityFloatProp(const edict_t* e, const char* name, float& out)
{
	edict_t* edict = const_cast<edict_t*>(e);
	if (edict == nullptr || edict->IsFree() || edict->GetUnknown() == nullptr)
		return false;
	IServerNetworkable* net = edict->GetUnknown()->GetNetworkable();
	ServerClass* cls = net != nullptr ? net->GetServerClass() : nullptr;
	if (cls == nullptr || cls->m_pTable == nullptr)
		return false;
	SendProp* prop = UTIL_FindSendProp(cls->m_pTable, name);
	unsigned offset = 0;
	if (prop == nullptr || prop->GetType() != DPT_Float ||
		!UTIL_FindSendPropInfo(cls, name, &offset) || offset == 0)
		return false;
	CClassInterfaceValue value(cls->m_pNetworkName, name, 0);
	value.findOffset();
	const float* ptr = value.getFloatPointer(edict);
	if (ptr == nullptr)
		return false;
	out = *ptr;
	return true;
}

bool ReadVehicleHealthFraction(const CBotEmpires* reader, const edict_t* vehicle, float& frac)
{
	frac = 1.0f;
	if (vehicle == nullptr || vehicle->IsFree())
		return false;
	float hp = 0.0f, maxHp = 0.0f;
	if (ReadEntityFloatProp(vehicle, "m_flTotalHealth", hp) &&
		ReadEntityFloatProp(vehicle, "m_flMaxTotalHealth", maxHp) &&
		maxHp > 0.0f)
	{
		frac = hp / maxHp;
		return true;
	}
	const int ihp = EmpiresVehicleReadInt(reader, vehicle, "m_iHealth", -1);
	const int imax = EmpiresVehicleReadInt(reader, vehicle, "m_iMaxHealth", -1);
	if (ihp >= 0 && imax > 0)
	{
		frac = static_cast<float>(ihp) / static_cast<float>(imax);
		return true;
	}
	return false;
}

int FindCrewSlot(const TeamLawlietState& st, int client)
{
	if (st.phase == PHASE_IDLE || client <= 0 || client >= kMaxClients)
		return -1;
	for (int i = 0; i < st.crewCount; ++i)
		if (st.crewClients[i] == client)
			return i;
	return -1;
}

void ResetBotAction(int client)
{
	if (client <= 0 || client >= kMaxClients)
		return;
	const int uid = s_bot[client].userId;
	s_bot[client] = BotLawlietAction();
	s_bot[client].userId = uid;
}

void ResetTeamToIdle(int team, TeamLawlietState& st, float cooldownNow)
{
	(void)team;
	for (int i = 0; i < st.crewCount; ++i)
		ResetBotAction(st.crewClients[i]);
	const float matchStart = st.matchStart;
	const float nextSkipLog = st.nextSkipLog;
	st = TeamLawlietState();
	st.matchStart = matchStart;
	st.nextSkipLog = nextSkipLog;
	if (cooldownNow > 0.0f)
		st.cooldownUntil = cooldownNow + kCooldownSeconds;
}

edict_t* FindEnemyCommandVehicle(const CBotEmpires* reader, int enemyTeam, int& entityIndex, Vector& origin)
{
	entityIndex = -1;
	if (engine == nullptr || gpGlobals == nullptr)
		return nullptr;
	const char* cvCls = enemyTeam == EMP_TEAM_NF ? "emp_nf_commander" : "emp_imp_commander";
	for (int i = gpGlobals->maxClients + 1; i < gpGlobals->maxEntities; ++i)
	{
		edict_t* e = engine->PEntityOfEntIndex(i);
		if (e == nullptr || e->IsFree() || e->GetClassName() == nullptr)
			continue;
		const bool match = IsClass(e, cvCls) ||
			(IsClass(e, "emp_vehicle") && EmpiresVehicleReadInt(reader, e, "m_iTeamNum", -1) == enemyTeam &&
			 IsCommanderChassis(EmpiresVehicleReadInt(reader, e, "m_iChassis", -1)));
		if (!match || !EmpiresVehicleAlive(reader, e))
			continue;
		entityIndex = i;
		origin = CBotGlobals::entityOrigin(e);
		return e;
	}
	return nullptr;
}

edict_t* LiveApcEdict(const CBotEmpires* reader, int team, int apcEntity, int apcSerial)
{
	if (engine == nullptr || gpGlobals == nullptr || apcEntity <= gpGlobals->maxClients ||
		apcEntity >= gpGlobals->maxEntities)
		return nullptr;
	edict_t* e = engine->PEntityOfEntIndex(apcEntity);
	if (!IsClass(e, "emp_vehicle") || (apcSerial > 0 && e->m_NetworkSerialNumber != apcSerial))
		return nullptr;
	if (EmpiresVehicleReadInt(reader, e, "m_iTeamNum", -1) != team ||
		!IsApcChassis(EmpiresVehicleReadInt(reader, e, "m_iChassis", -1)) ||
		!EmpiresVehicleAlive(reader, e))
		return nullptr;
	return e;
}

int CountFriendlyCratesNear(int team, const Vector& at, float radius, int& firstEntity, Vector& firstOrigin)
{
	firstEntity = -1;
	int count = 0;
	if (engine == nullptr || gpGlobals == nullptr)
		return 0;
	for (int i = gpGlobals->maxClients + 1; i < gpGlobals->maxEntities; ++i)
	{
		edict_t* e = engine->PEntityOfEntIndex(i);
		if (!IsClass(e, "emp_eng_crate"))
			continue;
		if (CClassInterface::getTeam(e) != team)
			continue;
		const Vector pos = CBotGlobals::entityOrigin(e);
		if (Dist2D(pos, at) <= radius)
		{
			if (firstEntity < 0)
			{
				firstEntity = i;
				firstOrigin = pos;
			}
			++count;
		}
	}
	return count;
}

// Own-team sticky / stun projectiles on the map: a new one after the release proves the throw
// (the grenade ammo is refilled near the APC, 235742: 2 -> 138, so a dropping count is no signal).
int CountTeamGrenadeProjectiles(bool stun, int team)
{
	const char* teamTag = team == EMP_TEAM_BE ? "_imp_" : "_nf_";
	int count = 0;
	for (int index = gpGlobals->maxClients + 1; index < gpGlobals->maxEntities; ++index)
	{
		edict_t* e = INDEXENT(index);
		const char* ec = e != nullptr && !e->IsFree() ? e->GetClassName() : nullptr;
		if (ec == nullptr || std::strstr(ec, teamTag) == nullptr ||
			std::strstr(ec, "sticky") == nullptr || std::strstr(ec, "_grenade_projectile") == nullptr)
			continue;
		if ((std::strstr(ec, "stun") != nullptr) == stun)
			++count;
	}
	return count;
}

// Grenade count: the game decrements CBaseCombatCharacter::m_iAmmo[type] (DecrementAmmo 0x38cf50 ->
// RemoveAmmo; GetAmmoCount 0x3174d0 reads +0x6bc). The generic CBot::getAmmo pointer (SendProp
// lookup) returned rising garbage for grenade type 17 (235742: 2 -> 138, never dropped).
int LawlietAmmo(const CBotEmpires* bot, edict_t* player, int ammoType)
{
	if (bot == nullptr || player == nullptr || ammoType < 0 || ammoType >= 32)
		return 0;
	const int generic = bot->getAmmo(static_cast<std::size_t>(ammoType));
	if (!EmpiresBinaryOffsetsTrusted() || player->GetUnknown() == nullptr)
		return generic;
	const CBaseEntity* base = player->GetUnknown()->GetBaseEntity();
	if (base == nullptr)
		return generic;
	const int real = *reinterpret_cast<const int*>(reinterpret_cast<const char*>(base) + 0x6bc + 4 * ammoType);
	static bool s_logged = false;
	if (!s_logged)
	{
		s_logged = true;
		const int* list = CClassInterface::getAmmoList(player);
		RCBOT_DIAG("[RCBOT2] lawliet ammo-read type=%d real=%d generic=%d generic_off=0x%x\n", ammoType, real, generic,
			list != nullptr ? static_cast<unsigned>(reinterpret_cast<const char*>(list) - reinterpret_cast<const char*>(base)) : 0u);
	}
	return real;
}

bool FindCarriedWeapon(const CBotEmpires* bot, edict_t* player, const char* needle, const char* exclude,
	const char*& exactName, int& ammoType, int& ammo)
{
	exactName = nullptr;
	ammoType = -1;
	ammo = 0;
	if (bot == nullptr || player == nullptr)
		return false;
	const CBaseHandle* weapons = CClassInterface::getWeaponList(player);
	if (weapons == nullptr)
		return false;
	for (int slot = 0; slot < MAX_WEAPONS; ++slot)
	{
		// Stale handles: after the last grenade is thrown the weapon slot's edict can be
		// reused by the projectile (emp_nf_sticky_grenade_projectile), whose fields are not
		// weapon fields (garbage ammo type/count in 230906). Check serial + weapon_ prefix.
		edict_t* w = EmpiresCarriedWeapon(weapons[slot]);
		if (w == nullptr)
			continue;
		const char* wc = w->GetClassName();
		if (!ContainsCaseInsensitive(wc, needle))
			continue;
		if (exclude != nullptr && ContainsCaseInsensitive(wc, exclude))
			continue;
		int secondary = -1;
		CClassInterface::getAmmoTypes(w, &ammoType, &secondary);
		exactName = wc;
		ammo = ammoType >= 0 ? LawlietAmmo(bot, player, ammoType) : 0;
		return true;
	}
	return false;
}

bool BuildingTeam(const CBotEmpires* reader, edict_t* e, const char* cls, int& team)
{
	team = EmpiresVehicleReadInt(reader, e, "m_iTeamNum", -1);
	if (team == EMP_TEAM_NF || team == EMP_TEAM_BE)
		return true;
	team = std::strncmp(cls, "emp_building_nf_", 16) == 0 ? EMP_TEAM_NF :
		(std::strncmp(cls, "emp_building_imp_", 17) == 0 ? EMP_TEAM_BE : -1);
	return team > 0;
}

void ApplyPreferredLoadoutCmds(edict_t* player, int team, int classId)
{
	if (helpers == nullptr || player == nullptr || player->IsFree())
		return;
	if (classId == EMP_CLASS_SCOUT)
	{
		helpers->ClientCommand(player, team == EMP_TEAM_NF ? "changegrenade 17\n" : "changegrenade 38\n");
		helpers->ClientCommand(player, team == EMP_TEAM_NF ? "changeequipment 20\n" : "changeequipment 41\n");
		helpers->ClientCommand(player, "acceptLoadout\n");
	}
	else if (classId == EMP_CLASS_RIFLEMAN)
	{
		helpers->ClientCommand(player, team == EMP_TEAM_NF ? "changegrenade 16\n" : "changegrenade 37\n");
		helpers->ClientCommand(player, team == EMP_TEAM_NF ? "changeequipment 21\n" : "changeequipment 42\n");
		helpers->ClientCommand(player, "acceptLoadout\n");
	}
}

Vector FactoryRallySpot(edict_t* factory)
{
	const Vector origin = CBotGlobals::entityOrigin(factory);
	float yaw = 0.0f;
	if (factory->GetCollideable() != nullptr)
		yaw = factory->GetCollideable()->GetCollisionAngles().y;
	const bool be = factory->GetClassName() != nullptr && std::strstr(factory->GetClassName(), "imp") != nullptr;
	const float rad = yaw * 3.14159265f / 180.0f;
	const Vector dir(std::cos(rad), std::sin(rad), 0.0f);
	Vector spot = origin + dir * (be ? 1550.0f : 1250.0f);
	Vector onMesh;
	if (RCBotNavMesh::IsLoaded() && RCBotNavMesh::ClosestPointOnMesh(spot, 400.0f, onMesh))
		spot = onMesh;
	return spot;
}

void UpdateEnemyCvStunState(TeamLawlietState& st, int team, edict_t* enemyCv, float now)
{
	if (enemyCv == nullptr || enemyCv->IsFree())
	{
		st.cvStunned = false;
		return;
	}
	float heat = 0.0f;
	const bool heatKnown = ReadEntityFloatProp(enemyCv, "m_flVehicleHeat", heat);
	float penaltyEnd = 0.0f;
	bool penaltyValid = false;
	if (EmpiresBinaryOffsetsTrusted() && enemyCv->GetUnknown() != nullptr)
	{
		if (const CBaseEntity* base = enemyCv->GetUnknown()->GetBaseEntity())
		{
			const float raw = *reinterpret_cast<const float*>(reinterpret_cast<const char*>(base) + 0x954);
			if (std::isfinite(raw) && raw > now && raw <= now + 30.0f)
			{
				penaltyEnd = raw;
				penaltyValid = true;
			}
		}
	}
	const bool throwFallback = st.lastStunThrowAt > 0.0f &&
		(now - st.lastStunThrowAt) >= kStunFuseFallbackSeconds &&
		(now - st.lastStunThrowAt) <= kStunFuseFallbackSeconds + 10.0f &&
		st.lastStunThrowDist <= 320.0f;

	const bool stunnedNow = penaltyValid || (heatKnown && heat >= 90.0f) || throwFallback;
	if (stunnedNow)
	{
		const bool newlyStunned = !st.cvStunned;
		st.cvStunned = true;
		st.cvStunnedEver = true;
		if (penaltyValid)
			st.cvStunUntil = penaltyEnd;
		else if (newlyStunned || st.cvStunUntil < now)
			st.cvStunUntil = now + 10.0f;
		if (newlyStunned)
		{
			const char* signal = penaltyValid ? "penalty" : (heatKnown && heat >= 90.0f ? "heat" : "throw-fallback");
			RCBOT_DIAG("[RCBOT2] lawliet cv-stunned t=%.1f team=%d cv=%d heat=%.0f penalty=%.1f signal=%s\n",
				now, team, st.enemyCvEntity, heat, penaltyValid ? penaltyEnd - now : -1.0f, signal);
		}
	}
	else if (now > st.cvStunUntil)
	{
		st.cvStunned = false;
	}
}
// Kill-spawn a crew member that is far from the rally point; the spawn anchor
// (EmpiresLawlietWantsSpawnAnchor) puts the respawn at the base/APC.
bool LawlietCrewRecall(const CBotEmpires* reader, int team, const TeamLawlietState& st, int client, const char* why)
{
	if (helpers == nullptr || !st.rallyValid)
		return false;
	edict_t* pe = engine->PEntityOfEntIndex(client);
	if (pe == nullptr || pe->IsFree() || playerinfomanager == nullptr)
		return false;
	IPlayerInfo* pi = playerinfomanager->GetPlayerInfo(pe);
	if (pi == nullptr || !pi->IsConnected() || !pi->IsFakeClient() || pi->GetTeamIndex() != team ||
		pi->IsDead() || pi->GetHealth() <= 0)
		return false;
	const int inVeh = EmpiresVehiclePlayerVehicle(reader, pe);
	if (inVeh >= 0 && inVeh == st.apcEntity)
		return false;
	const float dist = Dist2D(CBotGlobals::entityOrigin(pe), st.rallyPoint);
	// Seated in another vehicle: always respawn (he would not leave it on his own).
	if (dist <= kCrewWalkRadius && inVeh < 0)
		return false;
	RCBOT_DIAG("[RCBOT2] lawliet crew-recall team=%d client=%d why=%s dist=%.0f\n", team, client, why, dist);
	helpers->ClientCommand(pe, "kill\n");
	return true;
}

} // namespace

// Friend helper for clean access to CBotEmpires private property/navigation helpers.
struct EmpiresLawlietImpl
{
	static bool ApcHasFreeSeat(const CBotEmpires* reader, edict_t* apc)
	{
		if (apc == nullptr || apc->IsFree())
			return false;
		const int chassis = EmpiresVehicleReadInt(reader, apc, "m_iChassis", 1);
		const int seats = chassis >= 0 && chassis < 14 && kChassisSeats[chassis] > 0 ? kChassisSeats[chassis] : 8;
		for (int s = 0; s < seats; ++s)
		{
			int occupant = -1;
			if (reader->readEmpiresEntityHandleProperty(apc, "m_hPassengers", s, occupant) && occupant <= 0)
				return true;
		}
		return false;
	}

	static bool ConfirmOrRequestDriver(const CBotEmpires* reader, edict_t* apc, TeamLawlietState& st, int team)
	{
		int seat = -1;
		if (!reader->readEmpiresEntityHandleProperty(apc, "m_hPassengers", 0, seat))
			return false;
		for (int i = 0; i < st.crewCount; ++i)
		{
			const int client = st.crewClients[i];
			if (seat > 0 && seat != client)
				continue;
			edict_t* player = engine->PEntityOfEntIndex(client);
			IPlayerInfo* info = player != nullptr && !player->IsFree() && playerinfomanager != nullptr ?
				playerinfomanager->GetPlayerInfo(player) : nullptr;
			if (info == nullptr || !info->IsConnected() || !info->IsFakeClient() ||
				info->IsDead() || info->GetTeamIndex() != team)
				continue;
			if (seat == client)
			{
				st.driverClient = client;
				return true;
			}
			if (helpers != nullptr && EmpiresVehiclePlayerVehicle(reader, player) == st.apcEntity)
			{
				helpers->ClientCommand(player, "emp_change_seat 0\n");
				break;
			}
		}
		return false;
	}

	static bool IsEligibleBot(const CBotEmpires* reader, edict_t* e, int team, CBotEmpires*& outBot,
		IPlayerInfo*& outInfo, int& outClass)
	{
		outBot = nullptr;
		outInfo = nullptr;
		outClass = -1;
		if (e == nullptr || e->IsFree() || playerinfomanager == nullptr)
			return false;
		IPlayerInfo* info = playerinfomanager->GetPlayerInfo(e);
		if (info == nullptr || !info->IsConnected() || !info->IsFakeClient() || info->GetTeamIndex() != team ||
			EmpiresIsEngibot(e))
			return false;
		CBot* baseBot = CBots::getBotPointer(e);
		if (baseBot == nullptr)
			return false;
		CBotEmpires* emp = static_cast<CBotEmpires*>(baseBot);
		if (emp->commanderDutyBlocksSideTasks() || emp->m_iCommanderEntryStage != 0 ||
			emp->m_iCommanderBuildStage != 0 || emp->isEmpiresBillNye())
			return false;
		const int client = engine->IndexOfEdict(e);
		int order = 0;
		if (reader->readEmpiresIntProperty(e, "m_iCommandType", order) && order != 0 &&
			!EmpiresCommanderOrderIsOurs(team, client))
			return false;
		bool desired = false, desiredKnown = false;
		const char* prop = nullptr;
		if (!emp->readEmpiresClass(e, outClass, desired, desiredKnown, prop))
			outClass = emp->m_iDesiredClass;
		if (outClass < EMP_CLASS_SCOUT || outClass > EMP_CLASS_ENGINEER)
			return false;
		outBot = emp;
		outInfo = info;
		return true;
	}

	static void IssueFootNav(CBotEmpires* bot, BotLawlietAction& act, const Vector& goal, float now, bool force = false)
	{
		Vector target = goal;
		Vector onMesh;
		if (RCBotNavMesh::IsLoaded() && RCBotNavMesh::ClosestPointOnMesh(goal, 300.0f, onMesh))
			target = onMesh;
		const bool idle = bot->m_pSchedules == nullptr || bot->m_pSchedules->isEmpty();
		if (!force && !idle && now < act.nextNav && Dist2D(act.navGoal, target) < 200.0f)
			return;
		act.nextNav = now + 2.5f;
		act.navGoal = target;
		if (bot->m_bEmpiresNavGoalActive)
			bot->endEmpiresNavmeshGoal("drop", "lawliet", 0.0f);
		bot->cancelNavigationAttempt("lawliet");
		if (bot->m_pSchedules != nullptr && !bot->m_pSchedules->isEmpty())
			bot->m_pSchedules->freeMemory();
		bot->clearNavigationAttemptCooldownForStartAnchorRetry();
		if (!RCBotNavMesh::IsLoaded() || !bot->requestNavigation(target, RCBotNavigation::NavigationPolicy::NAVMESH_ONLY, 0))
			bot->setMoveTo(target);
	}

	static void IssueDirectMove(CBotEmpires* bot, const Vector& goal)
	{
		if (bot->m_bEmpiresNavGoalActive)
			bot->endEmpiresNavmeshGoal("drop", "lawliet", 0.0f);
		bot->cancelNavigationAttempt("lawliet");
		if (bot->m_pSchedules != nullptr && !bot->m_pSchedules->isEmpty())
			bot->m_pSchedules->freeMemory();
		bot->setMoveTo(goal);
	}

	static void ClearNavForAction(CBotEmpires* bot, const char* reason)
	{
		if (bot->m_bEmpiresNavGoalActive)
			bot->endEmpiresNavmeshGoal("drop", reason, 0.0f);
		bot->cancelNavigationAttempt(reason);
		if (bot->m_pSchedules != nullptr && !bot->m_pSchedules->isEmpty())
			bot->m_pSchedules->freeMemory();
		bot->stopMoving();
	}

	static void UpdateTeamCoordinator(CBotEmpires* reader, int team, float now)
	{
		TeamLawlietState& st = s_team[team];
		if (now < st.nextCoord)
			return;
		st.nextCoord = now + kCoordInterval;
		if (st.matchStart < 0.0f)
			st.matchStart = now;

		const int enemyTeam = team == EMP_TEAM_NF ? EMP_TEAM_BE : EMP_TEAM_NF;
		int enemyCvEntity = -1;
		Vector enemyCvOrigin;
		edict_t* enemyCv = FindEnemyCommandVehicle(reader, enemyTeam, enemyCvEntity, enemyCvOrigin);
		st.enemyCvAlive = enemyCv != nullptr;
		st.enemyCvEntity = enemyCvEntity;
		if (enemyCv != nullptr)
		{
			st.enemyCvOrigin = enemyCvOrigin;
			// 3.20 part B: with fair intel the rush heads for the team's known CV position
			// (live sight, last sighting, else the initial enemy base).
			Vector known;
			bool live = false;
			if (EmpiresFairIntelEnabled(team) && EmpiresTeamIntelEnemyBase(team, known, live))
				st.enemyCvOrigin = known;
		}

		const int cvarMode = rcbot_empires_lawliet.GetInt();
		if (st.phase != PHASE_IDLE && cvarMode == 0)
		{
			RCBOT_DIAG("[RCBOT2] lawliet end team=%d reason=cvar-off elapsed=%.0f\n", team, now - st.phaseSince);
			ResetTeamToIdle(team, st, 0.0f);
			return;
		}

		if (st.phase == PHASE_RUSHING || st.phase == PHASE_ASSAULT)
		{
			UpdateEnemyCvStunState(st, team, enemyCv, now);
			int crateEnt = -1;
			Vector cratePos;
			if (st.enemyCvAlive && CountFriendlyCratesNear(team, st.enemyCvOrigin, 600.0f, crateEnt, cratePos) > 0)
			{
				st.crateEntity = crateEnt;
				st.crateOrigin = cratePos;
			}
			else
				st.crateEntity = -1;
		}

		if (st.phase == PHASE_IDLE)
		{
			if (cvarMode == 0 || !st.enemyCvAlive)
				return;

			edict_t* vf = nullptr;
			Vector vfOrigin;
			int vfIndex = -1;
			const bool hasVf = reader->findEmpiresVehicleFactory(team, vf, vfOrigin, vfIndex);

			edict_t* existingApc = nullptr;
			int existingApcIndex = -1;
			int existingApcDriver = -1;
			for (int i = gpGlobals->maxClients + 1; i < gpGlobals->maxEntities; ++i)
			{
				edict_t* e = engine->PEntityOfEntIndex(i);
				if (!IsClass(e, "emp_vehicle") || EmpiresVehicleReadInt(reader, e, "m_iTeamNum", -1) != team ||
					!IsApcChassis(EmpiresVehicleReadInt(reader, e, "m_iChassis", -1)) ||
					!EmpiresVehicleAlive(reader, e))
					continue;
				float hpFrac = 1.0f;
				if (ReadVehicleHealthFraction(reader, e, hpFrac) && hpFrac < 0.50f)
					continue;
				int driver = -1;
				reader->readEmpiresEntityHandleProperty(e, "m_hPassengers", 0, driver);
				if (driver > 0 && driver <= gpGlobals->maxClients)
				{
					edict_t* dEdict = engine->PEntityOfEntIndex(driver);
					CBotEmpires* dBot = nullptr;
					IPlayerInfo* dInfo = nullptr;
					int dCls = -1;
					if (!IsEligibleBot(reader, dEdict, team, dBot, dInfo, dCls))
						continue;
				}
				else
				{
					const Vector apcAt = CBotGlobals::entityOrigin(e);
					bool humanNear = false;
					for (int p = 1; p <= gpGlobals->maxClients && !humanNear; ++p)
					{
						edict_t* pe = engine->PEntityOfEntIndex(p);
						IPlayerInfo* pi = pe != nullptr && !pe->IsFree() && playerinfomanager != nullptr ?
							playerinfomanager->GetPlayerInfo(pe) : nullptr;
						humanNear = pi != nullptr && pi->IsConnected() && !pi->IsFakeClient() &&
							!pi->IsDead() && pi->GetTeamIndex() == team &&
							Dist2D(CBotGlobals::entityOrigin(pe), apcAt) < 300.0f;
					}
					if (humanNear)
						continue;
				}
				existingApc = e;
				existingApcIndex = i;
				existingApcDriver = driver > 0 ? driver : -1;
				break;
			}

			const Vector rallyRef = existingApc != nullptr ? CBotGlobals::entityOrigin(existingApc) :
				(hasVf ? FactoryRallySpot(vf) : reader->getOrigin());
			CrewCandidate cands[kMaxClients];
			int candCount = 0;
			int eligibleScouts = 0;
			int seatedScout = -1; // fallback: a scout busy in another (non-CV) vehicle
			float seatedScoutDist = 0.0f;
			for (int i = 1; i <= gpGlobals->maxClients && i < kMaxClients; ++i)
			{
				edict_t* e = engine->PEntityOfEntIndex(i);
				CBotEmpires* emp = nullptr;
				IPlayerInfo* info = nullptr;
				int cls = -1;
				if (!IsEligibleBot(reader, e, team, emp, info, cls))
					continue;
				if (info->IsDead() || info->GetHealth() <= 0)
					continue;
				const int inVeh = EmpiresVehiclePlayerVehicle(reader, e);
				const bool isApcDriver = existingApcIndex > 0 && inVeh == existingApcIndex && existingApcDriver == i;
				if ((inVeh >= 0 && !isApcDriver) ||
					(!isApcDriver && emp->m_vehicle.stage != EmpiresVehicleStage::NONE))
				{
					edict_t* veh = inVeh > 0 ? engine->PEntityOfEntIndex(inVeh) : nullptr;
					const bool commanderSeat = veh != nullptr && !veh->IsFree() &&
						(!IsClass(veh, "emp_vehicle") || IsCommanderChassis(EmpiresVehicleReadInt(reader, veh, "m_iChassis", -1)));
					if (cls == EMP_CLASS_SCOUT && seatedScout < 0 && !commanderSeat)
					{
						seatedScout = i;
						seatedScoutDist = Dist2D(CBotGlobals::entityOrigin(e), rallyRef);
					}
					continue;
				}

				bool hasPreferred = false;
				if (cls == EMP_CLASS_SCOUT)
				{
					++eligibleScouts;
					const char* w = nullptr;
					int at = -1, am = 0;
					hasPreferred = FindCarriedWeapon(emp, e, "sticky_stun", nullptr, w, at, am);
				}
				else if (cls == EMP_CLASS_RIFLEMAN)
				{
					const char* w = nullptr;
					int at = -1, am = 0;
					hasPreferred = FindCarriedWeapon(emp, e, "sticky_grenade", "stun", w, at, am);
				}
				cands[candCount++] = {
					i,
					cls,
					hasPreferred,
					isApcDriver,
					Dist2D(CBotGlobals::entityOrigin(e), rallyRef)
				};
			}

			// No free scout (all driving, 010611): pull one out of his vehicle; he is recalled
			// (kill-spawn) at the start like the rest of the crew.
			if (eligibleScouts == 0 && seatedScout > 0 && candCount < kMaxClients)
			{
				cands[candCount++] = { seatedScout, EMP_CLASS_SCOUT, true, false, seatedScoutDist };
				++eligibleScouts;
			}

			DesperationInput in;
			in.team = team;
			in.cvarMode = cvarMode;
			in.vehiclesEnabled = rcbot_empires_vehicles.GetBool();
			in.enemyCvAlive = st.enemyCvAlive;
			in.hasVfOrApc = hasVf || existingApc != nullptr ||
				(rcbot_empires_vehicle_debug_buy.GetBool() && g_pCVar != nullptr &&
				 g_pCVar->FindVar("emp_sv_debug_build_vehicles") != nullptr &&
				 g_pCVar->FindVar("emp_sv_debug_build_vehicles")->GetInt() != 0);
			in.onCooldown = now < st.cooldownUntil;
			in.matchElapsed = now - st.matchStart;
			in.eligibleScouts = eligibleScouts;
			in.eligibleTotal = candCount;

			ConVar* completeVar = g_pCVar != nullptr ? g_pCVar->FindVar("emp_sv_research_complete") : nullptr;
			const bool allResearched = completeVar != nullptr && completeVar->GetBool();
			if (!allResearched)
			{
				bool ownRes[EmpiresCommanderEconomy::kResearchItemCount] = {};
				bool enemyRes[EmpiresCommanderEconomy::kResearchItemCount] = {};
				if (EmpiresReadTeamResearch(team, ownRes) && EmpiresReadTeamResearch(enemyTeam, enemyRes))
				{
					in.researchKnown = true;
					for (int r = 0; r < EmpiresCommanderEconomy::kResearchItemCount; ++r)
					{
						if (ownRes[r]) ++in.ownResearchCount;
						if (enemyRes[r]) ++in.enemyResearchCount;
					}
					in.ownHasMedTank = ownRes[EmpiresCommanderEconomy::kResearchMediumTank];
					in.enemyHasMedTank = enemyRes[EmpiresCommanderEconomy::kResearchMediumTank];
					in.ownHasHeavyTank = ownRes[EmpiresCommanderEconomy::kResearchHeavyTank];
					in.enemyHasHeavyTank = enemyRes[EmpiresCommanderEconomy::kResearchHeavyTank];
				}
			}

			if (const CBotMod* mod = CBotGlobals::getCurrentMod())
			{
				for (const CEmpiresMod::CapturePointObservation& pt :
					static_cast<const CEmpiresMod*>(mod)->getCapturePoints())
				{
					if (!pt.originKnown)
						continue;
					++in.totalFlags;
					if (pt.teamOwnerKnown)
					{
						if (pt.teamOwner == team) ++in.ownFlags;
						else if (pt.teamOwner == enemyTeam) ++in.enemyFlags;
					}
				}
			}

			const char* ownCvCls = team == EMP_TEAM_NF ? "emp_nf_commander" : "emp_imp_commander";
			for (int i = gpGlobals->maxClients + 1; i < gpGlobals->maxEntities; ++i)
			{
				edict_t* e = engine->PEntityOfEntIndex(i);
				if (e == nullptr || e->IsFree() || e->GetClassName() == nullptr)
					continue;
				const char* cls = e->GetClassName();
				if (IsClass(e, ownCvCls) ||
					(IsClass(e, "emp_vehicle") && EmpiresVehicleReadInt(reader, e, "m_iTeamNum", -1) == team &&
					 IsCommanderChassis(EmpiresVehicleReadInt(reader, e, "m_iChassis", -1))))
				{
					float frac = 1.0f;
					if (ReadVehicleHealthFraction(reader, e, frac))
						in.ownCvHealthShare = frac;
				}
				else if (std::strncmp(cls, "emp_building_", 13) == 0 && std::strstr(cls, "refinery") != nullptr)
				{
					int bTeam = -1;
					if (BuildingTeam(reader, e, cls, bTeam) && EmpiresVehicleReadInt(reader, e, "m_iHealth", 0) > 0)
					{
						if (bTeam == team) ++in.ownRefineries;
						else if (bTeam == enemyTeam) ++in.enemyRefineries;
					}
				}
			}

			const EmpiresTeamDirector::Board* ownBoard = EmpiresTeamDirectorBoard(team);
			const EmpiresTeamDirector::Board* enemyBoard = EmpiresTeamDirectorBoard(enemyTeam);
			if (ownBoard != nullptr && ownBoard->ticketsKnown)
			{
				in.ticketsKnown = true;
				in.ownTickets = ownBoard->tickets;
				in.ownTicketsMax = ownBoard->ticketsMax;
			}
			if (enemyBoard != nullptr && enemyBoard->ticketsKnown)
			{
				in.enemyTicketsKnown = true;
				in.enemyTickets = enemyBoard->tickets;
			}

			const char* reason = TriggerReason(in);
			if (reason == nullptr)
			{
				// Forced but no free scout (all in vehicles, 002946): say so instead of idling silently.
				if (cvarMode == team && eligibleScouts < 1 && now >= st.nextSkipLog)
				{
					st.nextSkipLog = now + 15.0f;
					RCBOT_DIAG("[RCBOT2] lawliet skip team=%d reason=no-free-scout candidates=%d\n", team, candCount);
				}
				return;
			}

			CrewSelection sel;
			if (!SelectCrew(cands, candCount, sel))
			{
				if (now >= st.nextSkipLog)
				{
					st.nextSkipLog = now + 15.0f;
					RCBOT_DIAG("[RCBOT2] lawliet skip team=%d reason=no-crew candidates=%d scouts=%d\n",
						team, candCount, eligibleScouts);
				}
				return;
			}

			st.crewCount = sel.count;
			for (int i = 0; i < sel.count; ++i)
			{
				st.crewClients[i] = sel.clients[i];
				st.crewRoles[i] = sel.roles[i];
				ResetBotAction(sel.clients[i]);
				edict_t* me = engine->PEntityOfEntIndex(sel.clients[i]);
				if (me != nullptr && !me->IsFree())
					ApplyPreferredLoadoutCmds(me, team, sel.roles[i] == ROLE_SCOUT ? EMP_CLASS_SCOUT :
						(sel.roles[i] == ROLE_RIFLEMAN ? EMP_CLASS_RIFLEMAN : -1));
			}
			st.driverClient = sel.driverClient;
			st.phaseSince = now;
			st.firstPassengerAt = -1.0f;
			st.scoutReassignTried = false;
			st.cvStunned = false;
			st.cvStunnedEver = false;
			st.cvStunUntil = 0.0f;
			st.lastStunThrowAt = -1.0e9f;
			st.lastStunThrowDist = 1.0e9f;
			st.crateEntity = -1;

			if (existingApc != nullptr && existingApcDriver == st.driverClient)
			{
				st.apcEntity = existingApcIndex;
				st.apcSerial = existingApc->m_NetworkSerialNumber;
				st.rallyPoint = CBotGlobals::entityOrigin(existingApc);
				st.rallyValid = true;
				st.phase = PHASE_BOARDING;
			}
			else
			{
				st.apcEntity = -1;
				st.apcSerial = -1;
				st.rallyPoint = rallyRef;
				st.rallyValid = true;
				st.phase = PHASE_ACQUIRE_APC;
			}
			RCBOT_DIAG("[RCBOT2] lawliet start team=%d reason=%s phase=%s driver=%d crew=%d scouts=%d rifle=%d eng=%d apc=%d\n",
				team, reason, PhaseName(st.phase), st.driverClient, st.crewCount, sel.scoutCount, sel.rifleCount,
				sel.engCount, st.apcEntity);
			if (hasVf || existingApc != nullptr)
				for (int i = 0; i < st.crewCount; ++i)
					LawlietCrewRecall(reader, team, st, st.crewClients[i], "start");
			return;
		}

		if (!st.enemyCvAlive)
		{
			RCBOT_DIAG("[RCBOT2] lawliet end team=%d reason=cv-destroyed elapsed=%.0f\n", team, now - st.phaseSince);
			ResetTeamToIdle(team, st, now);
			return;
		}

		auto validateDriver = [&]() -> bool
		{
			edict_t* dEdict = st.driverClient > 0 ? engine->PEntityOfEntIndex(st.driverClient) : nullptr;
			CBotEmpires* dBot = nullptr;
			IPlayerInfo* dInfo = nullptr;
			int dCls = -1;
			if (IsEligibleBot(reader, dEdict, team, dBot, dInfo, dCls) && !dInfo->IsDead() && dInfo->GetHealth() > 0)
				return true;
			int replacement = -1;
			int bestScore = 99;
			for (int i = 0; i < st.crewCount; ++i)
			{
				const int c = st.crewClients[i];
				if (c == st.driverClient)
					continue;
				edict_t* pe = engine->PEntityOfEntIndex(c);
				CBotEmpires* pb = nullptr;
				IPlayerInfo* pi = nullptr;
				int pc = -1;
				if (!IsEligibleBot(reader, pe, team, pb, pi, pc) || pi->IsDead() || pi->GetHealth() <= 0)
					continue;
				int score = 4;
				if (st.crewRoles[i] == ROLE_SUPPORT) score = 0;
				else if (st.crewRoles[i] == ROLE_RIFLEMAN) score = 1;
				else if (st.crewRoles[i] == ROLE_ENGINEER) score = 2;
				else if (i > 0) score = 3;
				if (score < bestScore)
				{
					bestScore = score;
					replacement = c;
				}
			}
			if (replacement <= 0)
				return false;
			st.driverClient = replacement;
			RCBOT_DIAG("[RCBOT2] lawliet driver-swap team=%d driver=%d phase=%s\n", team, st.driverClient, PhaseName(st.phase));
			return true;
		};

		if (st.phase == PHASE_ACQUIRE_APC)
		{
			if (!validateDriver())
			{
				RCBOT_DIAG("[RCBOT2] stall-escalate who=lawliet team=%d stage=2 reason=no-driver\n", team);
				ResetTeamToIdle(team, st, now);
				return;
			}
			edict_t* dEdict = engine->PEntityOfEntIndex(st.driverClient);
			const int inVeh = EmpiresVehiclePlayerVehicle(reader, dEdict);
			if (inVeh > 0)
			{
				edict_t* veh = engine->PEntityOfEntIndex(inVeh);
				int seat0 = -1;
				if (IsClass(veh, "emp_vehicle") && EmpiresVehicleReadInt(reader, veh, "m_iTeamNum", -1) == team &&
					IsApcChassis(EmpiresVehicleReadInt(reader, veh, "m_iChassis", -1)) &&
					EmpiresVehicleAlive(reader, veh) &&
					reader->readEmpiresEntityHandleProperty(veh, "m_hPassengers", 0, seat0) &&
					seat0 == st.driverClient)
				{
					st.apcEntity = inVeh;
					st.apcSerial = veh->m_NetworkSerialNumber;
					st.rallyPoint = CBotGlobals::entityOrigin(veh);
					st.rallyValid = true;
					st.phase = PHASE_BOARDING;
					st.phaseSince = now;
					st.firstPassengerAt = -1.0f;
					st.crewProgressAt = now;
					for (int k = 0; k < kMaxCrew; ++k)
						st.crewBestDist[k] = 1.0e9f;
					RCBOT_DIAG("[RCBOT2] lawliet phase team=%d ACQUIRE_APC->BOARDING apc=%d driver=%d\n",
						team, st.apcEntity, st.driverClient);
					// The APC now marks the rally point (also without a VF, e.g. debug-bought).
					for (int k = 0; k < st.crewCount; ++k)
						if (st.crewClients[k] != st.driverClient)
							LawlietCrewRecall(reader, team, st, st.crewClients[k], "apc-ready");
					return;
				}
			}
			if (now - st.phaseSince > kAcquireTimeout)
			{
				RCBOT_DIAG("[RCBOT2] stall-escalate who=lawliet team=%d stage=2 reason=acquire-timeout\n", team);
				ResetTeamToIdle(team, st, now);
			}
			return;
		}

		if (st.phase == PHASE_BOARDING)
		{
			edict_t* apc = LiveApcEdict(reader, team, st.apcEntity, st.apcSerial);
			if (apc == nullptr)
			{
				RCBOT_DIAG("[RCBOT2] lawliet end team=%d reason=apc-lost-boarding\n", team);
				ResetTeamToIdle(team, st, now);
				return;
			}
			st.rallyPoint = CBotGlobals::entityOrigin(apc);
			st.rallyValid = true;

			const bool driverSeated = ConfirmOrRequestDriver(reader, apc, st, team);

			int scoutsBoarded = 0;
			int totalBoarded = 0;
			int aliveCrew = 0;
			for (int i = 0; i < st.crewCount; ++i)
			{
				const int c = st.crewClients[i];
				edict_t* pe = engine->PEntityOfEntIndex(c);
				IPlayerInfo* pi = pe != nullptr && !pe->IsFree() && playerinfomanager != nullptr ?
					playerinfomanager->GetPlayerInfo(pe) : nullptr;
				if (pi == nullptr || !pi->IsConnected() || pi->GetTeamIndex() != team || pi->IsDead())
					continue;
				++aliveCrew;
				if (EmpiresVehiclePlayerVehicle(reader, pe) == st.apcEntity)
				{
					++totalBoarded;
					if (st.crewRoles[i] == ROLE_SCOUT)
						++scoutsBoarded;
				}
				else if (i < kMaxCrew)
				{
					// Progress = a missing crew member got clearly closer (walking or respawned nearby).
					const float d = Dist2D(CBotGlobals::entityOrigin(pe), CBotGlobals::entityOrigin(apc));
					if (d < st.crewBestDist[i] - kBoardingProgressStep)
					{
						if (st.crewBestDist[i] < 1.0e8f)
							st.crewProgressAt = now;
						st.crewBestDist[i] = d;
					}
				}
			}
			if (totalBoarded >= 2 && st.firstPassengerAt < 0.0f)
				st.firstPassengerAt = now;

			const BoardingAction action = EvaluateBoarding(driverSeated, scoutsBoarded, totalBoarded, aliveCrew,
				now - st.phaseSince, st.firstPassengerAt >= 0.0f ? now - st.firstPassengerAt : 0.0f,
				st.scoutReassignTried, now - st.crewProgressAt);

			if (action == BOARDING_GO_FULL || action == BOARDING_GO_PARTIAL)
			{
				if (action == BOARDING_GO_PARTIAL)
					RCBOT_DIAG("[RCBOT2] stall-escalate who=lawliet team=%d stage=1 reason=boarding-partial aboard=%d/%d scouts=%d\n",
						team, totalBoarded, aliveCrew, scoutsBoarded);
				st.phase = PHASE_RUSHING;
				st.phaseSince = now;
				st.bestCvDist = Dist2D(CBotGlobals::entityOrigin(apc), st.enemyCvOrigin);
				st.rushProgressAt = now;
				st.rushAnchor = CBotGlobals::entityOrigin(apc);
				RCBOT_DIAG("[RCBOT2] lawliet phase team=%d BOARDING->RUSHING apc=%d aboard=%d/%d scouts=%d\n",
					team, st.apcEntity, totalBoarded, aliveCrew, scoutsBoarded);
				return;
			}
			if (action == BOARDING_REASSIGN_SCOUT)
			{
				st.scoutReassignTried = true;
				RCBOT_DIAG("[RCBOT2] stall-escalate who=lawliet team=%d stage=1 reason=reassign-scout\n", team);
				for (int i = 1; i <= gpGlobals->maxClients && i < kMaxClients; ++i)
				{
					if (FindCrewSlot(st, i) >= 0)
						continue;
					edict_t* pe = engine->PEntityOfEntIndex(i);
					CBotEmpires* pb = nullptr;
					IPlayerInfo* pi = nullptr;
					int pc = -1;
					if (!IsEligibleBot(reader, pe, team, pb, pi, pc) || pi->IsDead() || pc != EMP_CLASS_SCOUT)
						continue;
					if (st.crewCount < kMaxCrew)
					{
						st.crewClients[st.crewCount] = i;
						st.crewRoles[st.crewCount] = ROLE_SCOUT;
						st.crewBestDist[st.crewCount] = 1.0e9f;
						++st.crewCount;
						ResetBotAction(i);
						ApplyPreferredLoadoutCmds(pe, team, EMP_CLASS_SCOUT);
						RCBOT_DIAG("[RCBOT2] lawliet scout-added team=%d client=%d\n", team, i);
						LawlietCrewRecall(reader, team, st, i, "scout-added");
						break;
					}
				}
				return;
			}
			if (action == BOARDING_ABORT)
			{
				RCBOT_DIAG("[RCBOT2] stall-escalate who=lawliet team=%d stage=2 reason=boarding-timeout aboard=%d/%d scouts=%d\n",
					team, totalBoarded, aliveCrew, scoutsBoarded);
				ResetTeamToIdle(team, st, now);
			}
			return;
		}

		if (st.phase == PHASE_RUSHING)
		{
			edict_t* apc = LiveApcEdict(reader, team, st.apcEntity, st.apcSerial);
			if (apc == nullptr)
			{
				bool crewNearCv = false;
				for (int i = 0; i < st.crewCount; ++i)
				{
					edict_t* pe = engine->PEntityOfEntIndex(st.crewClients[i]);
					if (pe != nullptr && !pe->IsFree() && CBotGlobals::entityIsAlive(pe) &&
						Dist2D(CBotGlobals::entityOrigin(pe), st.enemyCvOrigin) <= kPoppedApcFootRadius)
					{
						crewNearCv = true;
						break;
					}
				}
				if (crewNearCv)
				{
					st.apcEntity = -1;
					st.phase = PHASE_ASSAULT;
					st.phaseSince = now;
					RCBOT_DIAG("[RCBOT2] lawliet phase team=%d RUSHING->ASSAULT reason=apc-destroyed-near-cv\n", team);
				}
				else
				{
					RCBOT_DIAG("[RCBOT2] lawliet end team=%d reason=apc-destroyed\n", team);
					ResetTeamToIdle(team, st, now);
				}
				return;
			}

			ConfirmOrRequestDriver(reader, apc, st, team);

			const Vector apcPos = CBotGlobals::entityOrigin(apc);
			st.rallyPoint = apcPos;
			const float dist = Dist2D(apcPos, st.enemyCvOrigin);
			if (dist < st.bestCvDist - 300.0f)
			{
				st.bestCvDist = dist;
				st.rushProgressAt = now;
				st.rushAnchor = apcPos;
			}
			else if (Dist2D(apcPos, st.rushAnchor) >= 400.0f)
			{
				// The enemy CV retreats from a close vehicle (5.23): an APC still driving after it is
				// progress, not a stall (010631: rush-stalled at 3475 while chasing).
				st.bestCvDist = (std::min)(st.bestCvDist, dist);
				st.rushProgressAt = now;
				st.rushAnchor = apcPos;
			}
			float hpFrac = 1.0f;
			ReadVehicleHealthFraction(reader, apc, hpFrac);
			const bool lowHp = hpFrac < 0.30f;
			const bool stalledNear = dist <= kEarlyDisembarkRadius && (lowHp || (now - st.rushProgressAt > 8.0f));
			if (dist <= kDisembarkRadius)
			{
				st.phase = PHASE_ASSAULT;
				st.phaseSince = now;
				RCBOT_DIAG("[RCBOT2] lawliet phase team=%d RUSHING->ASSAULT dist=%.0f reason=arrived\n", team, dist);
				return;
			}
			if (stalledNear)
			{
				st.phase = PHASE_ASSAULT;
				st.phaseSince = now;
				RCBOT_DIAG("[RCBOT2] stall-escalate who=lawliet team=%d stage=1 reason=early-disembark-near-cv dist=%.0f hp=%.2f\n",
					team, dist, hpFrac);
				return;
			}
			if (now - st.rushProgressAt > kRushStallSeconds || now - st.phaseSince > kRushTimeout)
			{
				RCBOT_DIAG("[RCBOT2] stall-escalate who=lawliet team=%d stage=2 reason=rush-stalled dist=%.0f elapsed=%.0f\n",
					team, dist, now - st.phaseSince);
				ResetTeamToIdle(team, st, now);
			}
			return;
		}

		if (st.phase == PHASE_ASSAULT)
		{
			int aliveNear = 0;
			for (int i = 0; i < st.crewCount; ++i)
			{
				edict_t* pe = engine->PEntityOfEntIndex(st.crewClients[i]);
				if (pe == nullptr || pe->IsFree() || !CBotGlobals::entityIsAlive(pe))
					continue;
				if (EmpiresVehiclePlayerVehicle(reader, pe) >= 0 ||
					Dist2D(CBotGlobals::entityOrigin(pe), st.enemyCvOrigin) <= 2500.0f)
					++aliveNear;
			}
			edict_t* apc = st.apcEntity > 0 ? LiveApcEdict(reader, team, st.apcEntity, st.apcSerial) : nullptr;
			if (aliveNear == 0 && (now - st.phaseSince > 15.0f) && apc == nullptr)
			{
				RCBOT_DIAG("[RCBOT2] lawliet end team=%d reason=crew-wiped elapsed=%.0f\n", team, now - st.phaseSince);
				ResetTeamToIdle(team, st, now);
				return;
			}
			if (now - st.phaseSince > kAssaultTimeout)
			{
				RCBOT_DIAG("[RCBOT2] stall-escalate who=lawliet team=%d stage=2 reason=assault-timeout\n", team);
				ResetTeamToIdle(team, st, now);
			}
		}
	}
};

void EmpiresLawlietLevelReset()
{
	for (TeamLawlietState& st : s_team)
		st = TeamLawlietState();
	for (BotLawlietAction& act : s_bot)
		act = BotLawlietAction();
}

void EmpiresLawlietOnBotReset(int client)
{
	if (client <= 0 || client >= kMaxClients)
		return;
	s_bot[client] = BotLawlietAction();
}

void EmpiresLawlietOnClientDisconnect(int client)
{
	EmpiresLawlietOnBotReset(client);
	for (int team : {EMP_TEAM_NF, EMP_TEAM_BE})
	{
		TeamLawlietState& st = s_team[team];
		const int slot = FindCrewSlot(st, client);
		if (slot < 0)
			continue;
		if (st.driverClient == client)
			st.driverClient = -1;
		for (int i = slot + 1; i < st.crewCount; ++i)
		{
			st.crewClients[i - 1] = st.crewClients[i];
			st.crewRoles[i - 1] = st.crewRoles[i];
			st.crewBestDist[i - 1] = st.crewBestDist[i];
		}
		if (--st.crewCount == 0)
			ResetTeamToIdle(team, st, gpGlobals != nullptr ? gpGlobals->curtime : 0.0f);
	}
}

bool EmpiresLawlietActive(int team)
{
	return (team == EMP_TEAM_NF || team == EMP_TEAM_BE) && s_team[team].phase != PHASE_IDLE;
}

int EmpiresLawlietPhase(int team)
{
	return (team == EMP_TEAM_NF || team == EMP_TEAM_BE) ? s_team[team].phase : PHASE_IDLE;
}

bool EmpiresLawlietIsCrew(int team, int client)
{
	return (team == EMP_TEAM_NF || team == EMP_TEAM_BE) && FindCrewSlot(s_team[team], client) >= 0;
}

bool EmpiresLawlietIsDriver(int team, int client)
{
	return (team == EMP_TEAM_NF || team == EMP_TEAM_BE) && s_team[team].phase != PHASE_IDLE &&
		client > 0 && s_team[team].driverClient == client;
}

bool EmpiresLawlietWantsApcBuy(int team, int client)
{
	if (team != EMP_TEAM_NF && team != EMP_TEAM_BE)
		return false;
	const TeamLawlietState& st = s_team[team];
	return client > 0 && st.phase == PHASE_ACQUIRE_APC && st.driverClient == client;
}

bool EmpiresLawlietBlocksVehicleEntry(int team, int client)
{
	if (team != EMP_TEAM_NF && team != EMP_TEAM_BE)
		return false;
	const TeamLawlietState& st = s_team[team];
	if (FindCrewSlot(st, client) < 0)
		return false;
	if (st.phase == PHASE_ACQUIRE_APC)
		return st.driverClient != client;
	return st.phase == PHASE_BOARDING || st.phase == PHASE_RUSHING || st.phase == PHASE_ASSAULT;
}

bool EmpiresLawlietBlocksSideTasks(int team, int client)
{
	if (team != EMP_TEAM_NF && team != EMP_TEAM_BE)
		return false;
	const TeamLawlietState& st = s_team[team];
	const int slot = FindCrewSlot(st, client);
	if (slot < 0)
		return false;
	if (st.phase == PHASE_ACQUIRE_APC || st.phase == PHASE_BOARDING || st.phase == PHASE_RUSHING)
		return true;
	if (st.phase == PHASE_ASSAULT)
	{
		if (s_bot[client].mode != ACT_NONE)
			return true;
		if (st.crewRoles[slot] == ROLE_ENGINEER && st.crateEntity < 0)
			return true;
	}
	return false;
}

bool EmpiresLawlietStayInVehicle(int team, int client, int vehicleIndex)
{
	if (team != EMP_TEAM_NF && team != EMP_TEAM_BE)
		return false;
	const TeamLawlietState& st = s_team[team];
	return (st.phase == PHASE_BOARDING || st.phase == PHASE_RUSHING) &&
		vehicleIndex > 0 && st.apcEntity == vehicleIndex &&
		FindCrewSlot(st, client) >= 0;
}

bool EmpiresLawlietShouldDisembark(int team, int client, int vehicleIndex)
{
	if (team != EMP_TEAM_NF && team != EMP_TEAM_BE)
		return false;
	const TeamLawlietState& st = s_team[team];
	return st.phase == PHASE_ASSAULT && vehicleIndex > 0 && st.apcEntity == vehicleIndex &&
		FindCrewSlot(st, client) >= 0;
}

bool EmpiresLawlietOverrideDriving(int team, int client, int vehicleIndex, Vector& driveTo, float& arrive,
	float& throttle, bool& holdPosition)
{
	if (team != EMP_TEAM_NF && team != EMP_TEAM_BE)
		return false;
	const TeamLawlietState& st = s_team[team];
	if (FindCrewSlot(st, client) < 0 || vehicleIndex <= 0 || st.apcEntity != vehicleIndex)
		return false;
	if (st.phase == PHASE_BOARDING)
	{
		holdPosition = true;
		arrive = 300.0f;
		throttle = 0.0f;
		return true;
	}
	if (st.phase == PHASE_RUSHING && st.enemyCvAlive)
	{
		holdPosition = false;
		driveTo = st.enemyCvOrigin;
		arrive = 250.0f;
		throttle = 1.0f;
		return true;
	}
	return false;
}

bool EmpiresLawlietWantsSpawnAnchor(int team, int client, Vector& anchor, int& apcEntity)
{
	if (team != EMP_TEAM_NF && team != EMP_TEAM_BE)
		return false;
	const TeamLawlietState& st = s_team[team];
	if (FindCrewSlot(st, client) < 0)
		return false;
	if (st.phase == PHASE_ASSAULT && st.enemyCvAlive)
	{
		anchor = st.enemyCvOrigin;
		apcEntity = st.apcEntity;
		return true;
	}
	if ((st.phase == PHASE_BOARDING || st.phase == PHASE_RUSHING) && st.rallyValid)
	{
		anchor = st.rallyPoint;
		apcEntity = st.apcEntity;
		return true;
	}
	if (st.phase == PHASE_ACQUIRE_APC && st.rallyValid)
	{
		anchor = st.rallyPoint;
		apcEntity = -1;
		return true;
	}
	return false;
}

bool EmpiresLawlietBoardingGoal(int team, int client, Vector& goal)
{
	if (team != EMP_TEAM_NF && team != EMP_TEAM_BE)
		return false;
	const TeamLawlietState& st = s_team[team];
	if ((st.phase != PHASE_ACQUIRE_APC && st.phase != PHASE_BOARDING) || !st.rallyValid ||
		client == st.driverClient || FindCrewSlot(st, client) < 0)
		return false;
	goal = st.rallyPoint;
	return true;
}

bool EmpiresLawlietForceGrenadeLoadout(int team, int classId, int& preferredVariant)
{
	if (!EmpiresLawlietActive(team) && rcbot_empires_lawliet.GetInt() != team)
		return false;
	if (classId == EMP_CLASS_SCOUT)
	{
		preferredVariant = 0; // StickyStun variant
		return true;
	}
	if (classId == EMP_CLASS_RIFLEMAN)
	{
		preferredVariant = 1; // Sticky grenade variant
		return true;
	}
	return false;
}

void EmpiresLawlietNoteStunThrown(int team, int targetEntity, float dist)
{
	if ((team != EMP_TEAM_NF && team != EMP_TEAM_BE) || gpGlobals == nullptr)
		return;
	TeamLawlietState& st = s_team[team];
	if (st.phase != PHASE_IDLE && targetEntity == st.enemyCvEntity)
	{
		st.lastStunThrowAt = gpGlobals->curtime;
		st.lastStunThrowDist = dist;
	}
}

void CBotEmpires::tickEmpiresLawliet()
{
	if (gpGlobals == nullptr || engine == nullptr || m_pEdict == nullptr || m_pEdict->IsFree())
		return;
	const int team = getTeam();
	if (team != EMP_TEAM_NF && team != EMP_TEAM_BE)
		return;
	const float now = gpGlobals->curtime;
	EmpiresLawlietImpl::UpdateTeamCoordinator(this, team, now);

	const int client = engine->IndexOfEdict(m_pEdict);
	if (client <= 0 || client >= kMaxClients)
		return;
	const int uid = engine->GetPlayerUserId(m_pEdict);
	BotLawlietAction& act = s_bot[client];
	if (act.userId != uid)
	{
		act = BotLawlietAction();
		act.userId = uid;
	}
	const bool inVeh = EmpiresVehiclePlayerVehicle(this, m_pEdict) >= 0;
	if (!isAlive() || m_pPlayerInfo == nullptr || m_pPlayerInfo->IsDead() ||
		FindCrewSlot(s_team[team], client) < 0 || inVeh)
	{
		act.cmdOverride = false;
		act.mode = ACT_NONE;
	}
}

bool CBotEmpires::lawlietBlocksWeapons() const
{
	if (engine == nullptr || m_pEdict == nullptr || m_pEdict->IsFree())
		return false;
	const int client = engine->IndexOfEdict(m_pEdict);
	if (client <= 0 || client >= kMaxClients)
		return false;
	return s_bot[client].mode != ACT_NONE;
}

bool CBotEmpires::applyEmpiresLawlietCommand(CBotCmd& command)
{
	if (engine == nullptr || m_pEdict == nullptr || m_pEdict->IsFree())
		return false;
	const int client = engine->IndexOfEdict(m_pEdict);
	if (client <= 0 || client >= kMaxClients)
		return false;
	const BotLawlietAction& act = s_bot[client];
	if (!act.cmdOverride || act.userId != engine->GetPlayerUserId(m_pEdict))
		return false;
	command.forwardmove = act.cmdForward;
	command.sidemove = act.cmdSide;
	command.upmove = 0.0f;
	command.buttons = act.cmdButtons;
	command.viewangles = act.cmdView;
	return true;
}

bool CBotEmpires::handleEmpiresLawlietTask()
{
	if (gpGlobals == nullptr || engine == nullptr || m_pEdict == nullptr || m_pEdict->IsFree() ||
		!isAlive() || m_pPlayerInfo == nullptr || m_pPlayerInfo->IsDead())
		return false;
	const int team = getTeam();
	if (team != EMP_TEAM_NF && team != EMP_TEAM_BE)
		return false;
	TeamLawlietState& st = s_team[team];
	const int client = engine->IndexOfEdict(m_pEdict);
	const int slot = FindCrewSlot(st, client);
	if (slot < 0)
		return false;

	const int uid = engine->GetPlayerUserId(m_pEdict);
	BotLawlietAction& act = s_bot[client];
	if (act.userId != uid)
	{
		act = BotLawlietAction();
		act.userId = uid;
	}
	act.cmdOverride = false;

	const int inVeh = EmpiresVehiclePlayerVehicle(this, m_pEdict);
	if (inVeh >= 0)
	{
		act.mode = ACT_NONE;
		return false;
	}

	const float now = gpGlobals->curtime;
	const Vector origin = getOrigin();
	const Vector eye = getEyePosition();
	const int role = st.crewRoles[slot];

	// ---- Phase: Acquire APC (passengers rally outside VF / spawn point).
	if (st.phase == PHASE_ACQUIRE_APC)
	{
		if (st.driverClient == client)
		{
			act.mode = ACT_NONE;
			return false;
		}
		if (st.rallyValid && Dist2D(origin, st.rallyPoint) > 280.0f)
			EmpiresLawlietImpl::IssueFootNav(this, act, st.rallyPoint, now);
		else
			stopMoving();
		return true;
	}

	// ---- Phase: Boarding (non-driver crew walk to APC and enter via IN_USE).
	if (st.phase == PHASE_BOARDING)
	{
		edict_t* apc = LiveApcEdict(this, team, st.apcEntity, st.apcSerial);
		if (apc == nullptr || !EmpiresLawlietImpl::ApcHasFreeSeat(this, apc))
		{
			act.mode = ACT_NONE;
			stopMoving();
			return true;
		}
		const Vector center = EntityCenter(apc);
		const Vector nearest = NearestOnEntity(apc, eye);
		const float dist = Dist2D(origin, center);
		const float hullDist = (nearest - eye).Length2D();
		if (dist > 170.0f && hullDist > 95.0f)
		{
			act.mode = ACT_NONE;
			if (dist > 320.0f && RCBotNavMesh::IsLoaded())
				EmpiresLawlietImpl::IssueFootNav(this, act, nearest, now);
			else
				EmpiresLawlietImpl::IssueDirectMove(this, nearest);
			return true;
		}
		EmpiresLawlietImpl::ClearNavForAction(this, "lawliet-board");
		if (act.mode != ACT_BOARD_ENTER)
		{
			act.mode = ACT_BOARD_ENTER;
			act.stageAt = now;
		}
		act.cmdOverride = true;
		// Aim at the actual vehicle body rather than the bloated OBB corner (crossroads
		// 20261122-213339: crew stopped at hullDist=50, dist=250 aiming at empty air).
		VectorAngles(center - eye, act.cmdView);
		act.cmdView.z = 0.0f;
		act.cmdForward = (dist > 110.0f || hullDist > 16.0f) ? kMaxMove * 0.6f : 0.0f;
		act.cmdSide = (now - act.stageAt > 2.5f) ?
			((static_cast<int>(now) % 2) == 0 ? kMaxMove * 0.4f : -kMaxMove * 0.4f) : 0.0f;
		act.cmdButtons = 0;
		if (now >= act.nextAction)
		{
			act.cmdButtons = IN_USE;
			act.nextAction = now + 0.6f;
		}
		return true;
	}

	// ---- Phase: Rushing (on-foot crew near enemy CV move toward it; others stay ready).
	if (st.phase == PHASE_RUSHING)
	{
		act.mode = ACT_NONE;
		if (st.enemyCvAlive && Dist2D(origin, st.enemyCvOrigin) < 1800.0f)
		{
			EmpiresLawlietImpl::IssueFootNav(this, act, st.enemyCvOrigin, now);
			return true;
		}
		return false;
	}

	if (st.phase != PHASE_ASSAULT || !st.enemyCvAlive)
	{
		act.mode = ACT_NONE;
		return false;
	}

	edict_t* enemyCv = engine->PEntityOfEntIndex(st.enemyCvEntity);
	if (enemyCv == nullptr || enemyCv->IsFree() || !EmpiresVehicleAlive(this, enemyCv))
	{
		act.mode = ACT_NONE;
		return false;
	}
	const Vector cvOrigin = CBotGlobals::entityOrigin(enemyCv);
	const Vector cvCenter = EntityCenter(enemyCv);
	const Vector cvNearest = NearestOnEntity(enemyCv, eye);
	const float cvDist = Dist2D(origin, cvCenter);
	const float cvHullDist = (cvNearest - eye).Length2D();

	// ---- Active Sub-Action A: Grenade Throw (Scout Stun or Rifleman Sticky).
	if (act.mode == ACT_THROW_GRENADE)
	{
		if (now - act.stageAt > 5.0f)
		{
			act.mode = ACT_NONE;
			act.cooldownUntil = now + 1.0f;
			return true;
		}
		stopMoving();
		Vector aim = cvCenter;
		aim.z += (std::min)(110.0f, cvDist * 0.22f);
		act.cmdOverride = true;
		VectorAngles(aim - eye, act.cmdView);
		act.cmdView.z = 0.0f;
		act.cmdForward = 0.0f;
		act.cmdSide = 0.0f;
		act.cmdButtons = 0;
		setLookVector(aim);
		setLookAtTask(LOOK_VECTOR, 0.2f);

		if (!act.thrown)
		{
			if (act.pressUntil <= 0.0f)
			{
				const edict_t* active = CClassInterface::getCurrentWeapon(m_pEdict);
				const char* activeCls = active != nullptr && !active->IsFree() ? active->GetClassName() : nullptr;
				if (activeCls == nullptr || !ContainsCaseInsensitive(activeCls, "sticky"))
				{
					selectWeaponName(act.weaponName);
					act.readyAt = 0.0f;
					return true;
				}
				// Wait for the draw animation: a press right after the switch is ignored by the
				// game (233608: 11 "throws", ammo never dropped, cv_hp 1.00).
				if (act.readyAt <= 0.0f)
				{
					act.readyAt = now + 0.8f;
					float nextAttack = 0.0f;
					if (ReadEntityFloatProp(active, "m_flNextPrimaryAttack", nextAttack) &&
						nextAttack > now && nextAttack < now + 3.0f)
						act.readyAt = (std::max)(now + 0.2f, nextAttack + 0.05f);
				}
				if (now < act.readyAt)
					return true;
				act.pressUntil = now + 0.45f;
				act.projBefore = CountTeamGrenadeProjectiles(act.isStunThrow, team);
			}
			const int curAmmoBeforeRelease = act.ammoType >= 0 ? LawlietAmmo(this, m_pEdict, act.ammoType) : act.ammoBefore;
			if (!(act.ammoBefore > 0 && curAmmoBeforeRelease < act.ammoBefore) && now < act.pressUntil)
			{
				act.cmdButtons = IN_ATTACK;
				return true;
			}
			act.cmdButtons = 0;
			act.thrown = true;
			act.observeUntil = now + 1.5f;
			return true;
		}
		const int curAmmo = act.ammoType >= 0 ? LawlietAmmo(this, m_pEdict, act.ammoType) : act.ammoBefore;
		const bool ammoDropped = act.ammoBefore > 0 && curAmmo < act.ammoBefore;
		const int projNow = CountTeamGrenadeProjectiles(act.isStunThrow, team);
		const bool confirmed = ammoDropped || (act.projBefore >= 0 && projNow > act.projBefore);
		if (!confirmed && now < act.observeUntil)
			return true;
		// Only a real throw restarts the re-stun clock; a failed press retries after the cooldown.
		if (act.isStunThrow && confirmed)
			EmpiresLawlietNoteStunThrown(team, st.enemyCvEntity, cvDist);
		float cvHpFrac = -1.0f;
		ReadVehicleHealthFraction(this, enemyCv, cvHpFrac);
		// Diagnostics (230906/233608: "thrown" logged with rising ammo and cv_hp 1.00):
		// active weapon, its clip, and own grenade projectiles near the CV prove a real throw.
		const edict_t* activeWep = CClassInterface::getCurrentWeapon(m_pEdict);
		const char* activeWepCls = activeWep != nullptr && !activeWep->IsFree() ? activeWep->GetClassName() : nullptr;
		int projectiles = 0;
		for (int index = gpGlobals->maxClients + 1; index < gpGlobals->maxEntities; ++index)
		{
			edict_t* e = INDEXENT(index);
			const char* ec = e != nullptr && !e->IsFree() ? e->GetClassName() : nullptr;
			if (ec != nullptr && std::strstr(ec, "_grenade_projectile") != nullptr &&
				Dist2D(CBotGlobals::entityOrigin(e), cvCenter) < 700.0f)
				++projectiles;
		}
		// Armor absorbs damage before hull HP: sum of the four sides (emp_vehicle +0xfe4, as in
		// bot_empires_vehicle.cpp VehicleArmorSides).
		int cvArmor = -1;
		if (EmpiresBinaryOffsetsTrusted() && enemyCv->GetUnknown() != nullptr)
			if (const CBaseEntity* cvBase = enemyCv->GetUnknown()->GetBaseEntity())
			{
				const float* sides = reinterpret_cast<const float*>(reinterpret_cast<const char*>(cvBase) + 0xfe4);
				cvArmor = static_cast<int>(sides[0] + sides[1] + sides[2] + sides[3]);
			}
		// DecrementAmmo (0x38cf50) takes the ammo type from weapon +0x4c0; compare with the SendProp type.
		int gameType = -1, gameAmmo = -1;
		if (EmpiresBinaryOffsetsTrusted() && activeWep != nullptr && !activeWep->IsFree() &&
			const_cast<edict_t*>(activeWep)->GetUnknown() != nullptr && m_pEdict->GetUnknown() != nullptr)
		{
			const CBaseEntity* wb = const_cast<edict_t*>(activeWep)->GetUnknown()->GetBaseEntity();
			const CBaseEntity* pb = m_pEdict->GetUnknown()->GetBaseEntity();
			if (wb != nullptr && pb != nullptr)
			{
				gameType = *reinterpret_cast<const int*>(reinterpret_cast<const char*>(wb) + 0x4c0);
				if (gameType >= 0 && gameType < 32)
					gameAmmo = *reinterpret_cast<const int*>(reinterpret_cast<const char*>(pb) + 0x6bc + 4 * gameType);
			}
		}
		RCBOT_DIAG("[RCBOT2] lawliet %s-%s t=%.1f userid=%d team=%d cv=%d dist=%.0f ammo=%d->%d type=%d game_type=%d game_ammo=%d dropped=%d active=%s proj=%d->%d near=%d cv_hp=%.2f cv_armor=%d\n",
			act.isStunThrow ? "stun" : "sticky", confirmed ? "thrown" : "press-failed", now, uid, team, st.enemyCvEntity, cvDist,
			act.ammoBefore, curAmmo, act.ammoType, gameType, gameAmmo, ammoDropped ? 1 : 0, activeWepCls != nullptr ? activeWepCls : "-",
			act.projBefore, projNow, projectiles, cvHpFrac, cvArmor);
		act.mode = ACT_NONE;
		act.cooldownUntil = now + 0.5f;
		return true;
	}

	// ---- Active Sub-Action B: Engineer Ammo Crate Placement beside Enemy CV.
	if (act.mode == ACT_PLACE_CRATE)
	{
		int crateEnt = -1;
		Vector cratePos;
		if (act.subStage == 4 && CountFriendlyCratesNear(team, origin, 350.0f, crateEnt, cratePos) > act.crateBaseline)
		{
			st.crateEntity = crateEnt;
			st.crateOrigin = cratePos;
			RCBOT_DIAG("[RCBOT2] lawliet ammo-crate placed userid=%d team=%d cv=%d crate=%d dist=%.0f\n",
				uid, team, st.enemyCvEntity, crateEnt, Dist2D(cratePos, cvOrigin));
			act.mode = ACT_NONE;
			act.cooldownUntil = now + 1.0f;
			return true;
		}
		if (now - act.stageAt > (act.subStage == 1 ? 12.0f : 6.0f))
		{
			RCBOT_DIAG("[RCBOT2] stall-escalate who=lawliet-crate userid=%d stage=1 reason=timeout\n", uid);
			act.mode = ACT_NONE;
			act.cooldownUntil = now + 5.0f;
			return true;
		}
		if (act.subStage == 1 && act.crateMove)
		{
			const float left = Dist2D(origin, act.crateStand);
			if (left > 45.0f && now - act.stageAt < 8.0f)
			{
				if (left < 220.0f || !RCBotNavMesh::IsLoaded())
					EmpiresLawlietImpl::IssueDirectMove(this, act.crateStand);
				else
					EmpiresLawlietImpl::IssueFootNav(this, act, act.crateStand, now);
				return true;
			}
			act.crateMove = false;
			act.subStage = 2;
			act.stageAt = now;
		}
		stopMoving();
		const char* kit = team == EMP_TEAM_NF ? "weapon_emp_nf_engineer_kit" : "weapon_emp_imp_engineer_kit";
		const edict_t* cur = CClassInterface::getCurrentWeapon(m_pEdict);
		const bool kitActive = cur != nullptr && !cur->IsFree() && cur->GetClassName() != nullptr &&
			std::strcmp(cur->GetClassName(), kit) == 0;
		if (act.subStage == 2)
		{
			if (!kitActive)
			{
				selectWeaponName(kit);
				return true;
			}
			int item = -1;
			readEmpiresDebugIntProperty(m_pEdict, "m_iEngineerBuildItem", item);
			if (item != 7)
			{
				if (now >= act.nextAction && helpers != nullptr)
				{
					act.nextAction = now + 0.5f;
					helpers->ClientCommand(m_pEdict, "engSelect 7\n");
				}
				return true;
			}
			Vector dir = origin - cvOrigin;
			dir.z = 0.0f;
			dir = dir.Length() > 1.0f ? dir.Normalized() : Vector(1, 0, 0);
			const float baseAngle = (75.0f + 45.0f * static_cast<float>(act.crateTries)) * 3.14159265f / 180.0f;
			dir = Vector(dir.x * std::cos(baseAngle) - dir.y * std::sin(baseAngle),
				dir.x * std::sin(baseAngle) + dir.y * std::cos(baseAngle), 0.0f);
			act.crateTarget = Vector(eye.x, eye.y, origin.z) + dir * 80.0f;
			act.crateBaseline = CountFriendlyCratesNear(team, origin, 350.0f, crateEnt, cratePos);
			act.subStage = 3;
			act.stageAt = now;
		}

		act.cmdOverride = true;
		VectorAngles(act.crateTarget - eye, act.cmdView);
		act.cmdView.z = 0.0f;
		act.cmdForward = 0.0f;
		act.cmdSide = 0.0f;
		act.cmdButtons = 0;
		setLookVector(act.crateTarget);
		setLookAtTask(LOOK_VECTOR, 0.2f);

		if (act.subStage == 3)
		{
			if (now - act.stageAt < 0.4f || !kitActive)
				return true;
			act.cmdButtons = IN_ATTACK;
			if (now - act.stageAt < 0.75f)
				return true;
			act.subStage = 4;
			act.stageAt = now;
			return true;
		}
		if (now - act.stageAt < 2.0f)
			return true;
		if (++act.crateTries >= 4)
		{
			RCBOT_DIAG("[RCBOT2] stall-escalate who=lawliet-crate userid=%d stage=2 reason=no-crate\n", uid);
			act.mode = ACT_NONE;
			act.cooldownUntil = now + 10.0f;
			return true;
		}
		Vector side = origin - cvOrigin;
		side.z = 0.0f;
		side = side.Length() > 1.0f ? side.Normalized() : Vector(1, 0, 0);
		const float turn = 120.0f * 3.14159265f / 180.0f;
		side = Vector(side.x * std::cos(turn) - side.y * std::sin(turn),
			side.x * std::sin(turn) + side.y * std::cos(turn), 0.0f);
		act.crateStand = cvOrigin + side * 240.0f;
		if (RCBotNavMesh::IsLoaded())
		{
			Vector mesh;
			if (RCBotNavMesh::ClosestPointOnMesh(act.crateStand, 180.0f, mesh))
				act.crateStand = mesh;
		}
		act.crateMove = true;
		act.subStage = 1;
		act.stageAt = now;
		return true;
	}

	// ---- Active Sub-Action C: Infantry Crate Resupply.
	if (act.mode == ACT_RESUPPLY_CRATE)
	{
		edict_t* crate = st.crateEntity > 0 ? engine->PEntityOfEntIndex(st.crateEntity) : nullptr;
		if (!IsClass(crate, "emp_eng_crate"))
		{
			act.mode = ACT_NONE;
			act.cooldownUntil = now + 2.0f;
			return true;
		}
		const Vector cPos = CBotGlobals::entityOrigin(crate);
		const float cDist = Dist2D(origin, cPos);
		const int curAmmo = act.ammoType >= 0 ? LawlietAmmo(this, m_pEdict, act.ammoType) : act.ammoBefore;
		if (curAmmo > act.ammoBefore)
		{
			RCBOT_DIAG("[RCBOT2] lawliet resupply userid=%d team=%d role=%s crate=%d ammo=%d->%d\n",
				uid, team, RoleName(role), st.crateEntity, act.ammoBefore, curAmmo);
			act.mode = ACT_NONE;
			act.cooldownUntil = now + 0.5f;
			return true;
		}
		if (now - act.stageAt > 10.0f)
		{
			RCBOT_DIAG("[RCBOT2] stall-escalate who=lawliet-resupply userid=%d stage=1 reason=no-ammo-gain ammo=%d\n",
				uid, curAmmo);
			act.mode = ACT_NONE;
			act.cooldownUntil = now + 4.0f;
			return true;
		}
		if (cDist > 72.0f)
		{
			if (cDist <= 300.0f || !RCBotNavMesh::IsLoaded())
				EmpiresLawlietImpl::IssueDirectMove(this, cPos);
			else
				EmpiresLawlietImpl::IssueFootNav(this, act, cPos, now);
			return true;
		}
		EmpiresLawlietImpl::ClearNavForAction(this, "lawliet-resupply");
		if (act.subStage == 0)
		{
			act.subStage = 1;
			act.stageAt = now;
			act.cmdOverride = true;
			VectorAngles((cPos + Vector(0, 0, 24)) - eye, act.cmdView);
			act.cmdView.z = 0.0f;
			act.cmdForward = 0.0f;
			act.cmdSide = 0.0f;
			act.cmdButtons = 0;
			return true;
		}
		act.cmdOverride = true;
		VectorAngles((cPos + Vector(0, 0, 24)) - eye, act.cmdView);
		act.cmdView.z = 0.0f;
		act.cmdForward = 0.0f;
		act.cmdSide = 0.0f;
		act.cmdButtons = IN_USE;
		if (now - act.stageAt > 2.5f)
		{
			RCBOT_DIAG("[RCBOT2] stall-escalate who=lawliet-resupply userid=%d stage=1 reason=no-ammo-gain ammo=%d\n",
				uid, curAmmo);
			act.mode = ACT_NONE;
			act.cooldownUntil = now + 4.0f;
		}
		return true;
	}

	// ---- Role Dispatch in PHASE_ASSAULT (mode == ACT_NONE)
	const bool crateExists = st.crateEntity > 0;
	if (role == ROLE_ENGINEER)
	{
		if (!crateExists && now >= act.cooldownUntil)
		{
			float nextSupply = 0.0f;
			const bool supplyReady = !readEmpiresFloatProperty(m_pEdict, "m_flEngineerNextSupply", nextSupply) ||
				nextSupply <= now;
			if (cvDist > 320.0f)
			{
				Vector delta = cvOrigin - origin;
				delta.z = 0.0f;
				Vector approach = delta.Length2D() > 1.0f ?
					cvOrigin - delta.Normalized() * 220.0f : cvOrigin + Vector(220.0f, 0.0f, 0.0f);
				if (cvDist <= 420.0f || !RCBotNavMesh::IsLoaded())
					EmpiresLawlietImpl::IssueDirectMove(this, approach);
				else
					EmpiresLawlietImpl::IssueFootNav(this, act, approach, now);
				if (cvDist <= 450.0f && supplyReady)
				{
					const char* kit = team == EMP_TEAM_NF ? "weapon_emp_nf_engineer_kit" : "weapon_emp_imp_engineer_kit";
					selectWeaponName(kit);
				}
				return true;
			}
			if (supplyReady && (st.cvStunned || st.cvStunnedEver))
			{
				EmpiresLawlietImpl::ClearNavForAction(this, "lawliet-crate");
				act.mode = ACT_PLACE_CRATE;
				act.subStage = 2;
				act.stageAt = now;
				act.nextAction = 0.0f;
				act.crateTries = 0;
				act.crateMove = false;
				return true;
			}
			stopMoving();
			return true;
		}
		if (cvDist > 420.0f)
		{
			EmpiresLawlietImpl::IssueFootNav(this, act, cvOrigin, now);
			return true;
		}
		return false;
	}

	// After the final grenade the weapon entity can disappear (run 230906).
	// Keep using the ammo pool learned from this crew member's grenade.
	auto beginResupply = [&](int ammoType, int ammo) {
		if (!crateExists || now < act.cooldownUntil)
			return false;
		if (ammoType < 0 && act.weaponName[0] != '\0' &&
			((role == ROLE_SCOUT && act.isStunThrow) || (role == ROLE_RIFLEMAN && !act.isStunThrow)))
		{
			ammoType = act.ammoType;
			ammo = LawlietAmmo(this, m_pEdict, ammoType);
		}
		if (ammoType < 0 || ammoType >= 32 || ammo != 0)
			return false;
		act.mode = ACT_RESUPPLY_CRATE;
		act.subStage = 0;
		act.stageAt = now;
		act.ammoType = ammoType;
		act.ammoBefore = ammo;
		return true;
	};

	if (role == ROLE_SCOUT)
	{
		const char* stunName = nullptr;
		int stunAmmoType = -1, stunAmmo = 0;
		const bool hasStun = FindCarriedWeapon(this, m_pEdict, "sticky_stun", nullptr, stunName, stunAmmoType, stunAmmo);
		if (beginResupply(stunAmmoType, stunAmmo))
			return true;
		if (hasStun && stunAmmo > 0 && now >= act.cooldownUntil &&
			ShouldThrowStun(st.cvStunned, st.cvStunUntil - now, now - st.lastStunThrowAt))
		{
			if (cvDist > 260.0f && cvHullDist > 130.0f)
			{
				if (cvDist <= 420.0f || !RCBotNavMesh::IsLoaded())
					EmpiresLawlietImpl::IssueDirectMove(this, cvNearest);
				else
					EmpiresLawlietImpl::IssueFootNav(this, act, cvNearest, now);
				if (cvDist <= 500.0f)
					selectWeaponName(stunName);
				return true;
			}
			EmpiresLawlietImpl::ClearNavForAction(this, "lawliet-stun");
			act.mode = ACT_THROW_GRENADE;
			act.isStunThrow = true;
			snprintf(act.weaponName, sizeof(act.weaponName), "%s", stunName);
			act.ammoType = stunAmmoType;
			act.ammoBefore = stunAmmo;
			act.stageAt = now;
			act.pressUntil = 0.0f;
			act.readyAt = 0.0f;
			act.observeUntil = 0.0f;
			act.projBefore = -1;
			act.thrown = false;
			selectWeaponName(act.weaponName);
			return true;
		}
		if (cvDist > 420.0f)
			EmpiresLawlietImpl::IssueFootNav(this, act, cvNearest, now);
		else
			stopMoving();
		if (now >= act.nextAction)
		{
			act.nextAction = now + 3.0f;
			RCBOT_DIAG("[RCBOT2] lawliet scout-wait t=%.1f userid=%d has_stun=%d ammo=%d stunned=%d remain=%.1f since=%.1f cooldown=%.1f dist=%.0f\n",
				now, uid, hasStun ? 1 : 0, stunAmmo, st.cvStunned ? 1 : 0, st.cvStunUntil - now,
				now - st.lastStunThrowAt, act.cooldownUntil - now, cvDist);
		}
		return true;
	}

	if (role == ROLE_RIFLEMAN)
	{
		const char* stickyName = nullptr;
		int stickyAmmoType = -1, stickyAmmo = 0;
		const bool hasSticky = FindCarriedWeapon(this, m_pEdict, "sticky_grenade", "stun",
			stickyName, stickyAmmoType, stickyAmmo);
		if (beginResupply(stickyAmmoType, stickyAmmo))
			return true;
		if (hasSticky && stickyAmmo > 0 && now >= act.cooldownUntil)
		{
			if (cvDist > 320.0f && cvHullDist > 200.0f)
			{
				if (cvDist <= 450.0f || !RCBotNavMesh::IsLoaded())
					EmpiresLawlietImpl::IssueDirectMove(this, cvNearest);
				else
					EmpiresLawlietImpl::IssueFootNav(this, act, cvNearest, now);
				if (cvDist <= 550.0f)
					selectWeaponName(stickyName);
				return true;
			}
			EmpiresLawlietImpl::ClearNavForAction(this, "lawliet-sticky");
			act.mode = ACT_THROW_GRENADE;
			act.isStunThrow = false;
			snprintf(act.weaponName, sizeof(act.weaponName), "%s", stickyName);
			act.ammoType = stickyAmmoType;
			act.ammoBefore = stickyAmmo;
			act.stageAt = now;
			act.pressUntil = 0.0f;
			act.readyAt = 0.0f;
			act.observeUntil = 0.0f;
			act.projBefore = -1;
			act.thrown = false;
			selectWeaponName(act.weaponName);
			return true;
		}
		if (cvDist > 420.0f)
		{
			EmpiresLawlietImpl::IssueFootNav(this, act, cvNearest, now);
			return true;
		}
		stopMoving();
		return true;
	}

	const char* rpgName = nullptr;
	int rpgAmmoType = -1, rpgAmmo = 0;
	FindCarriedWeapon(this, m_pEdict, "_rpg", nullptr, rpgName, rpgAmmoType, rpgAmmo);
	if (beginResupply(rpgAmmoType, rpgAmmo))
		return true;
	if (cvDist > 700.0f)
	{
		EmpiresLawlietImpl::IssueFootNav(this, act, cvOrigin, now);
		return true;
	}
	return false;
}
