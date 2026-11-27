// Empires vehicles: buy, enter, drive, fight, run over, Bill Nye.
// Game facts (inputs, purchase commands, chassis/engine indices, stun grenade):
// reverse-engineering/notes/vehicle-driving.md.

#include "bot_empires.h"
#include "bot_empires_binary.h"
#include "bot_empires_team_economy.h"
#include "bot_empires_lawliet.h"
#include "bot_empires_wreck_clear.h"
#include "bot_empires_vehicle_aim.h"
#include "bot_empires_vehicle_context_steer.h"
#include "bot_buttons.h"
#include "bot_cvars.h"
#include "bot_globals.h"
#include "bot_mods.h"
#include "bot_getprop.h"
#include "server_class.h"
#include "bot_navmesh_api.h"
#include "navmesh/util/UtilTrace.h"
#include "bot_mtrand.h"
#include "bot_schedule.h"
#include "in_buttons.h"
#include "engine/IEngineTrace.h"
#include "engine/IStaticPropMgr.h"
#include "iplayerinfo.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

bool EmpiresTeamResources(int team, float& resources);  // bot_empires_commander_economy.cpp

extern IPlayerInfoManager* playerinfomanager;
extern IServerGameEnts* servergameents;
extern IServerTools* servertools;
extern IStaticPropMgrServer* staticpropmgr;
bool CGameRulesShouldCollide(int collisionGroup0, int collisionGroup1); // navmesh/util/UtilTrace.cpp
int g_exitBlockUserId = -1; // test-only, rcbotd empires_test_exit_block (bot_commands.cpp); 0 = first APC driver
int g_exitBlockSpawnUserId = -1; // the bot respawned by the passenger test skips the APC yield once
bool g_exitBlockPassenger = false; // ... passenger: a teammate respawns into the boxed vehicle

namespace
{
ConVar rcbot_empires_vehicle_turn_test("rcbot_empires_vehicle_turn_test", "0", 0,
	"Debug purchases only: 1 normal turn, 2 handbrake pulse, 3 coast straight, 4 brake straight");
ConVar rcbot_empires_apc_cover_park("rcbot_empires_apc_cover_park", "1", 0,
	"8.4: a spawn APC without a contested flag parks out of sight 1000-2300 HU before the nearest enemy building (1000-1800 HU at flags; else enemy CV) instead of driving into it");
ConVar rcbot_empires_vehicle_spread_wait("rcbot_empires_vehicle_spread_wait", "1", 0,
	"Each vehicle claims its front support / APC park / staging spot; other vehicles of the team skip spots "
	"within 500 HU of a claim (DMZ human test 27.11.: two tanks got the same front spot and wedged)");
ConVar rcbot_empires_apc_retreat_hull("rcbot_empires_apc_retreat_hull", "0.6", 0,
	"5.14a: a spawn APC retreats to the repair pad below this hull share (tanks 0.4); it also leaves a park spot "
	"where it lost 15 % of hull+armor within 15 s, and never drives into a fight like a tank");
ConVar rcbot_empires_vehicle_missiles("rcbot_empires_vehicle_missiles", "1", 0,
	"3.10f: tanks buy the Standard Missile Launcher (fire group 1 only); the driver selects group 1 "
	"(cannon+missile+MG) against vehicles and buildings and group 2 (cannon+MG) otherwise");
ConVar rcbot_empires_vehicle_cannon_eye("rcbot_empires_vehicle_cannon_eye", "1", 0,
	"3.21a: aim turret weapons from the cannon pivot (all weapons converge on the cannon axis); 0 = hull origin + 90");
ConVar rcbot_empires_vehicle_turret_align("rcbot_empires_vehicle_turret_align", "1", 0,
	"3.21a: fire once the turret yaw is within 8 deg (MG on infantry) or 12 deg (cannon) of the target and the aim is older than 0.4 s, else after 2.5 s with a stall-escalate who=vehicle-aim line; chassis without a rotatable turret keep the 1.2 s delay. 0 = always 1.2 s");
ConVar rcbot_empires_vehicle_pitch_limit("rcbot_empires_vehicle_pitch_limit", "1", 0,
	"3.21a: hold fire while the game clamps the cannon pitch (target below the chassis minimum range) and back off");
ConVar rcbot_empires_vehicle_keep_route("rcbot_empires_vehicle_keep_route", "1", 0,
	"Vehicles: when replanning finds no path to the same goal, keep driving the last route instead of straight at the goal");
ConVar rcbot_empires_vehicle_ctx_ground_slopes("rcbot_empires_vehicle_ctx_ground_slopes", "1", 0,
	"9.8: context steering and its passage scan treat level-trace hits on drivable ground (normal z >= 0.7) as free; 0 = a rising slope counts as a wall");
ConVar rcbot_empires_cv_no_path_stop("rcbot_empires_cv_no_path_stop", "1", 0,
	"9.8: a commander CV drive with no vehicle path (and no last route to keep) ends with reason=no-path instead of driving straight at the goal across slopes and shore");
ConVar rcbot_empires_vehicle_deep_water("rcbot_empires_vehicle_deep_water", "1", 0,
	"Vehicles: steering edge checks treat water deeper than the vehicle can ford as a drop");
ConVar rcbot_empires_vehicle_hold("rcbot_empires_vehicle_hold", "0", 0,
	"Test only: bought vehicles wait in place (fire at targets) until set to 0, for a fair start of both teams");
ConVar rcbot_empires_vehicle_shell_trace("rcbot_empires_vehicle_shell_trace", "0", 0,
	"Diagnosis: log where cannon shells/missiles end while a driver bot has a target");
ConVar rcbot_empires_vehicle_curve_speed("rcbot_empires_vehicle_curve_speed", "1", 0,
	"Anticipatory curve speed along the route for all chassis (arc-length Menger curvature); 0 = off");
ConVar rcbot_empires_vehicle_maneuvers("rcbot_empires_vehicle_maneuvers", "0", 0,
	"Experimental local maneuver selection for NF Light and BE AFV on ordinary routes");
ConVar rcbot_empires_vehicle_context_steer("rcbot_empires_vehicle_context_steer", "1", 0,
	"Context steering on routes: 16 directions scored by route interest and hull-width traces (walls, buildings); "
	"in front of a tight passage follow its axis and slow down until lined up; only at tight passages. 0 = off");
ConVar rcbot_empires_vehicle_maneuver_budget("rcbot_empires_vehicle_maneuver_budget", "2", 0,
	"Local maneuver evaluations per server tick for all vehicles together (others wait a tick)");
// SendPropArray3 arrays are datatables whose numbered children have relative offsets.
template <typename T, std::size_t N>
bool ReadTurretArray(edict_t* turret, const char* name, T (&values)[N], bool write = false)
{
	if (turret == nullptr || turret->IsFree() || turret->GetUnknown() == nullptr)
		return false;
	IServerUnknown* unknown = turret->GetUnknown();
	IServerNetworkable* networkable = unknown->GetNetworkable();
	ServerClass* cls = networkable != nullptr ? networkable->GetServerClass() : nullptr;
	CBaseEntity* entity = unknown->GetBaseEntity();
	if (cls == nullptr || cls->m_pTable == nullptr || entity == nullptr)
		return false;
	SendProp* driver = UTIL_FindSendProp(cls->m_pTable, "tankturretdriverdata");
	unsigned driverOffset = 0;
	if (driver == nullptr || driver->GetType() != DPT_DataTable || driver->GetDataTable() == nullptr ||
		!UTIL_FindSendPropInfo(cls, "tankturretdriverdata", &driverOffset))
		return false;
	SendProp* array = nullptr;
	for (int i = 0; i < driver->GetDataTable()->GetNumProps(); ++i)
	{
		SendProp* candidate = driver->GetDataTable()->GetProp(i);
		if (candidate != nullptr && candidate->GetName() != nullptr && std::strcmp(candidate->GetName(), name) == 0)
		{
			array = candidate;
			break;
		}
	}
	if (array == nullptr || array->GetType() != DPT_DataTable || array->GetDataTable() == nullptr ||
		array->GetDataTable()->GetNumProps() != static_cast<int>(N) || array->GetOffset() < 0)
		return false;
	const unsigned offset = driverOffset + static_cast<unsigned>(array->GetOffset());
	for (std::size_t i = 0; i < N; ++i)
	{
		SendProp* child = array->GetDataTable()->GetProp(i);
		char expected[4];
		snprintf(expected, sizeof(expected), "%03u", static_cast<unsigned>(i));
		if (child == nullptr || child->GetName() == nullptr || std::strcmp(child->GetName(), expected) != 0 ||
			child->GetType() != DPT_Int || child->GetOffset() != static_cast<int>(i * sizeof(T)))
			return false;
	}
	char* base = reinterpret_cast<char*>(entity) + offset;
	for (std::size_t i = 0; i < N; ++i)
	{
		if (write)
		{
			std::memcpy(base + i * sizeof(T), &values[i], sizeof(T)); // test command only
			continue;
		}
		T value;
		std::memcpy(&value, base + i * sizeof(T), sizeof(value));
		if (sizeof(T) == 1 && value > 1)
			return false;
		values[i] = value;
	}
	return true;
}

// Float send prop of the entity's own server class (readEmpiresFloatProperty
// looks only in CEmpPlayer, so vehicle health and heat never read).
float* EntityFloatProp(const edict_t* e, const char* name)
{
	edict_t* edict = const_cast<edict_t*>(e);
	if (edict == nullptr || edict->IsFree() || edict->GetUnknown() == nullptr)
		return nullptr;
	IServerNetworkable* net = edict->GetUnknown()->GetNetworkable();
	ServerClass* cls = net != nullptr ? net->GetServerClass() : nullptr;
	if (cls == nullptr || cls->m_pTable == nullptr)
		return nullptr;
	SendProp* prop = UTIL_FindSendProp(cls->m_pTable, name);
	unsigned offset = 0;
	if (prop == nullptr || prop->GetType() != DPT_Float ||
		!UTIL_FindSendPropInfo(cls, name, &offset) || offset == 0)
		return nullptr;
	CClassInterfaceValue value(cls->m_pNetworkName, name, 0);
	value.findOffset();
	return value.getFloatPointer(edict);
}

bool ReadEntityFloat(const edict_t* e, const char* name, float& out)
{
	const float* p = EntityFloatProp(e, name);
	if (p == nullptr)
		return false;
	out = *p;
	return true;
}

// Vehicle health as the game uses it: CBaseEntity::m_iHealth (+0xf8), read by
// GetHealth() in CEmpTank::DoRepairs and the engineer kit. m_flTotalHealth is
// a different value. Send prop if the class has one, else the field offset.
int* VehicleHealthField(const edict_t* e)
{
	edict_t* edict = const_cast<edict_t*>(e);
	if (edict == nullptr || edict->IsFree() || edict->GetUnknown() == nullptr)
		return nullptr;
	IServerNetworkable* net = edict->GetUnknown()->GetNetworkable();
	ServerClass* cls = net != nullptr ? net->GetServerClass() : nullptr;
	if (cls != nullptr && cls->m_pTable != nullptr)
	{
		SendProp* prop = UTIL_FindSendProp(cls->m_pTable, "m_iHealth");
		unsigned offset = 0;
		if (prop != nullptr && prop->GetType() == DPT_Int &&
			UTIL_FindSendPropInfo(cls, "m_iHealth", &offset) && offset != 0)
		{
			CClassInterfaceValue value(cls->m_pNetworkName, "m_iHealth", 0);
			value.findOffset();
			if (int* p = value.getIntPointer(edict))
				return p;
		}
	}
	if (!EmpiresBinaryOffsetsTrusted())
		return nullptr;
	CBaseEntity* base = edict->GetUnknown()->GetBaseEntity();
	return base != nullptr ? reinterpret_cast<int*>(reinterpret_cast<char*>(base) + 0xf8) : nullptr;
}

bool ReadVehicleHealth(const edict_t* e, float& out)
{
	const int* p = VehicleHealthField(e);
	if (p == nullptr)
		return false;
	out = static_cast<float>(*p);
	return true;
}

bool ReadVehicleMaxHealth(const edict_t* e, float& out)
{
	edict_t* edict = const_cast<edict_t*>(e);
	if (edict == nullptr || edict->IsFree() || edict->GetUnknown() == nullptr)
		return false;
	CBaseEntity* base = edict->GetUnknown()->GetBaseEntity();
	const datamap_t* map = base != nullptr ? CBaseEntity_GetDataDescMap(base) : nullptr;
	const unsigned offset = map != nullptr ? UTIL_FindInDataMap(map, "m_iMaxHealth") : 0;
	if (offset == 0)
		return false;
	const int maxHp = *reinterpret_cast<const int*>(reinterpret_cast<const char*>(base) + offset);
	if (maxHp <= 0)
		return false;
	out = static_cast<float>(maxHp);
	return true;
}

// Armor per side (CEmpTank::SetArmorSide 0x68e8f0): float[4] at +0xfe4, a
// rounded int[4] mirror at +0x1004, max m_iMaxArmorHealth (+0x1024). Server
// only, no send props. Side order as emp_vehicle_armor_levels: front, rear,
// left, right. Returns false without an armored CEmpTank.
float* VehicleArmorSides(const edict_t* e)
{
	edict_t* edict = const_cast<edict_t*>(e);
	if (edict == nullptr || edict->IsFree() || edict->GetUnknown() == nullptr ||
		edict->GetClassName() == nullptr || std::strcmp(edict->GetClassName(), "emp_vehicle") != 0)
		return nullptr;
	if (!EmpiresBinaryOffsetsTrusted())
		return nullptr;
	CBaseEntity* base = edict->GetUnknown()->GetBaseEntity();
	return base != nullptr ? reinterpret_cast<float*>(reinterpret_cast<char*>(base) + 0xfe4) : nullptr;
}

int ReadArmorSide(const edict_t* e, int side)
{
	const float* sides = VehicleArmorSides(e);
	return sides != nullptr && side >= 0 && side < 4 ? static_cast<int>(sides[side]) : -1;
}



constexpr float kPi = 3.14159265f;
constexpr float kMaxMove = 368.0f;          // forwardmove/sidemove full scale (UpdateDriverControls)
constexpr float kEnterDistance = 170.0f;
constexpr float kRoadProbeUp = 80.0f;
constexpr float kRoadProbeDown = 360.0f;    // deeper than this below the vehicle = a fall

// Diagnostic for the factory purchase (raw offsets, trusted binary only): player +0x1454
// = factory handle (-1 cleared by CVehicleFactory::EndTouch 0x6a9ec2), factory +0x492 = built,
// +0x60c = curtime of the last successful CanBuild (0x6aa2f4).
// Probe filter: skips players, one entity and collision group 12 (vehicle physbox, which
// ShouldCollide(0,12) lets through, 0x3fd829) — what the game's purchase traces ignore.
class FactoryProbeFilter : public CTraceFilter
{
public:
	explicit FactoryProbeFilter(edict_t* skip) : m_skip(skip) {}
	bool ShouldHitEntity(IHandleEntity* handle, int) override
	{
		IServerUnknown* unknown = static_cast<IServerUnknown*>(handle);
		IServerNetworkable* networkable = unknown != nullptr ? unknown->GetNetworkable() : nullptr;
		edict_t* edict = networkable != nullptr ? networkable->GetEdict() : nullptr;
		if (edict == nullptr)
			return true;
		const int index = engine->IndexOfEdict(edict);
		if (edict == m_skip || (index >= 1 && index <= gpGlobals->maxClients))
			return false;
		ICollideable* collide = unknown->GetCollideable();
		return collide == nullptr || collide->GetCollisionGroup() != 12;
	}
private:
	edict_t* m_skip;
};

const char* ProbeHitName(const trace_t& t)
{
	if (t.m_pEnt == nullptr)
		return "none";
	IServerNetworkable* n = reinterpret_cast<IServerUnknown*>(t.m_pEnt)->GetNetworkable();
	edict_t* e = n != nullptr ? n->GetEdict() : nullptr;
	return e != nullptr && e->GetClassName() != nullptr ? e->GetClassName() : "?";
}

// CEmpPlayer::m_flWages, player +0x12c8 (RE vehicle-driving.md "Wages and team pool"). -1 = unknown.
float EmpiresPlayerWages(edict_t* player)
{
	if (!EmpiresBinaryOffsetsTrusted() || player == nullptr || player->GetUnknown() == nullptr)
		return -1.0f;
	const char* p = reinterpret_cast<const char*>(player->GetUnknown()->GetBaseEntity());
	if (p == nullptr)
		return -1.0f;
	const float wages = *reinterpret_cast<const float*>(p + 0x12c8);
	return wages >= 0.0f && wages < 1.0e7f ? wages : -1.0f;
}

// A human teammate asks for wages in chat ("wages" anywhere in the text, user 14.11.): every
// bot of his team with wages hands them over. With the servers' give plugin loaded (chat
// trigger !give <name> = console command sm_give) the bot uses it like a player would;
// without it the wages are moved directly (player +0x12c8, network state marked changed).
void HandleWagesChat(edict_t* speaker, const char* text, bool testBotSpeaker)
{
	if (speaker == nullptr || text == nullptr || (!testBotSpeaker && CBots::getBotPointer(speaker) != nullptr) ||
		CBotGlobals::getCurrentMod() == nullptr || CBotGlobals::getCurrentMod()->getBotType() != BOTTYPE_EMPIRES ||
		!EmpiresBinaryOffsetsTrusted() || helpers == nullptr || playerinfomanager == nullptr)
		return;
	bool asked = false;
	for (const char* c = text; *c != '\0' && !asked; ++c)
		asked = std::tolower(static_cast<unsigned char>(c[0])) == 'w' && c[1] != '\0' &&
			std::tolower(static_cast<unsigned char>(c[1])) == 'a' && c[2] != '\0' &&
			std::tolower(static_cast<unsigned char>(c[2])) == 'g' && c[3] != '\0' &&
			std::tolower(static_cast<unsigned char>(c[3])) == 'e';
	if (!asked)
		return;
	const int team = CClassInterface::getTeam(speaker);
	IPlayerInfo* info = playerinfomanager->GetPlayerInfo(speaker);
	if ((team != 2 && team != 3) || info == nullptr || info->GetName() == nullptr)
		return;
	const int speakerId = engine->GetPlayerUserId(speaker);
	static float s_nextAllowed[RCBOT_MAXPLAYERS + 1] = {};
	const int speakerIndex = engine->IndexOfEdict(speaker);
	if (speakerIndex <= 0 || speakerIndex > RCBOT_MAXPLAYERS || (gpGlobals->curtime < s_nextAllowed[speakerIndex] &&
		s_nextAllowed[speakerIndex] - gpGlobals->curtime <= 10.0f)) // curtime restarts on a new map
		return;
	s_nextAllowed[speakerIndex] = gpGlobals->curtime + 10.0f;

	// SourceMod target by #userid: a plain name can match several players (expert 14.11.).
	char target[32];
	snprintf(target, sizeof(target), "#%d", speakerId);
	const bool givePlugin = g_pCVar != nullptr && g_pCVar->FindCommandBase("sm_give") != nullptr;

	char* to = reinterpret_cast<char*>(speaker->GetUnknown() != nullptr ? speaker->GetUnknown()->GetBaseEntity() : nullptr);
	if (to == nullptr)
		return;
	for (int slot = 0; slot < RCBOT_MAXPLAYERS; ++slot)
	{
		CBot* bot = CBots::getBot(slot);
		edict_t* e = bot != nullptr && bot->inUse() ? bot->getEdict() : nullptr;
		if (e == nullptr || e == speaker || CClassInterface::getTeam(e) != team)
			continue;
		const float wages = EmpiresPlayerWages(e);
		if (wages < 1.0f)
			continue;
		if (givePlugin)
		{
			char command[96];
			snprintf(command, sizeof(command), "say !give %s\n", target);
			helpers->ClientCommand(e, command);
		}
		else
		{
			char* from = reinterpret_cast<char*>(e->GetUnknown()->GetBaseEntity());
			*reinterpret_cast<float*>(to + 0x12c8) += wages;
			*reinterpret_cast<float*>(from + 0x12c8) = 0.0f;
			e->m_fStateFlags |= FL_EDICT_CHANGED | FL_FULL_EDICT_CHANGED; // StateChanged() needs engine-only symbols
			speaker->m_fStateFlags |= FL_EDICT_CHANGED | FL_FULL_EDICT_CHANGED; // StateChanged() needs engine-only symbols
		}
		RCBOT_DIAG("[RCBOT2] wages-give requester=%d bot=%d amount=%.0f via=%s target=%s requester_wages=%.0f bot_wages=%.0f\n",
			speakerId, engine->GetPlayerUserId(e), wages, givePlugin ? "sm_give" : "direct", target,
			EmpiresPlayerWages(speaker), EmpiresPlayerWages(e));
	}
}

// 5.5a step 1: wages and team pool around a purchase; the difference before/after is the
// price and shows what paid (the game's vehicle_purchase event does not reach our hook).
void LogEmpiresVehicleEcon(const char* phase, int userId, int team, edict_t* player)
{
	float pool = -1.0f;
	EmpiresTeamResources(team, pool);
	const float wages = EmpiresPlayerWages(player);
	EmpiresTeamEconomy::TeamState& state = EmpiresTeamEconomy::Team(team);
	if (std::strcmp(phase, "before") == 0)
	{
		state.pendingUser = userId;
		state.poolBefore = pool;
		state.wagesBefore = wages;
	}
	else if (state.pendingUser == userId)
		EmpiresTeamEconomy::LearnVehicleCost(state, pool, wages);
	RCBOT_DIAG("[RCBOT2] vehicle econ phase=%s userid=%d team=%d wages=%.0f pool=%.0f reserve=%.0f cost=%.0f\n", phase,
		userId, team, wages, pool, EmpiresTeamEconomy::ActiveReserve(state, gpGlobals->curtime), state.vehicleCost);
}

void LogEmpiresFactoryProbe(const char* phase, int userId, edict_t* player, int factoryIndex)
{
	if (!rcbot_empires_debug_log.GetBool() || !EmpiresBinaryOffsetsTrusted() || player == nullptr || engine == nullptr)
		return;
	edict_t* factory = factoryIndex > 0 ? engine->PEntityOfEntIndex(factoryIndex) : nullptr;
	IServerUnknown* pu = player->GetUnknown();
	IServerUnknown* fu = factory != nullptr && !factory->IsFree() ? factory->GetUnknown() : nullptr;
	const char* p = reinterpret_cast<const char*>(pu != nullptr ? pu->GetBaseEntity() : nullptr);
	const char* f = reinterpret_cast<const char*>(fu != nullptr ? fu->GetBaseEntity() : nullptr);
	RCBOT_DIAG("[RCBOT2] vehicle factory-probe phase=%s userid=%d handle=0x%08x factory=%d built=%d can_build_at=%.1f curtime=%.1f\n",
		phase, userId, p != nullptr ? *reinterpret_cast<const unsigned*>(p + 0x1454) : 0u, factoryIndex,
		f != nullptr ? int(*reinterpret_cast<const unsigned char*>(f + 0x492)) : -1,
		f != nullptr ? *reinterpret_cast<const float*>(f + 0x60c) : -1.0f, gpGlobals->curtime);

	// emp_vehicle_build rejects when its ±150 hull at the factory's "build" attachment hits a
	// solid non-player that is not collision group 14 (0x6543c6..0x654ac0, expert 14.11.).
	// The attachment is unknown here: list solid entities within the factory bounds + 150.
	if (std::strcmp(phase, "buy") != 0 || factory == nullptr || factory->GetCollideable() == nullptr)
		return;
	{
		// The game's two checks, rebuilt: blocked hull ±150 at attachment "build" (BE (0,0,200),
		// NF (172.5,0,172.5) local, mask 0x4600400b) and IsInFactory (player hull 1024 down,
		// mask 0x200400b, must hit the factory first).
		const bool nf = CClassInterface::getTeam(factory) == 2;
		matrix3x4_t toWorld;
		AngleMatrix(factory->GetCollideable()->GetCollisionAngles(), factory->GetCollideable()->GetCollisionOrigin(), toWorld);
		Vector build;
		VectorTransform(nf ? Vector(172.5f, 0.0f, 172.5f) : Vector(0.0f, 0.0f, 200.0f), toWorld, build);
		Ray_t blockRay;
		blockRay.Init(build, build, Vector(-150.0f, -150.0f, -150.0f), Vector(150.0f, 150.0f, 150.0f));
		FactoryProbeFilter blockFilter(factory);
		trace_t block;
		enginetrace->TraceRay(blockRay, 0x4600400b, &blockFilter, &block);
		const Vector po = CBotGlobals::entityOrigin(player);
		Ray_t downRay;
		downRay.Init(po, po - Vector(0.0f, 0.0f, 1024.0f), Vector(-16.0f, -16.0f, 0.0f), Vector(16.0f, 16.0f, 72.0f));
		FactoryProbeFilter downFilter(player);
		trace_t down;
		enginetrace->TraceRay(downRay, 0x200400b, &downFilter, &down);
		RCBOT_DIAG("[RCBOT2] vehicle factory-check build=(%.0f %.0f %.0f) blocked_hit=%s blocked_solid=%d blocked_frac=%.2f "
			"me=(%.0f %.0f %.0f) under_hit=%s under_frac=%.3f under_solid=%d\n",
			build.x, build.y, build.z, ProbeHitName(block), block.startsolid ? 1 : 0, block.fraction,
			po.x, po.y, po.z, ProbeHitName(down), down.fraction, down.startsolid ? 1 : 0);
	}
	Vector fmins, fmaxs;
	factory->GetCollideable()->WorldSpaceSurroundingBounds(&fmins, &fmaxs);
	fmins -= Vector(150.0f, 150.0f, 150.0f);
	fmaxs += Vector(150.0f, 150.0f, 150.0f);
	const Vector fo = CBotGlobals::entityOrigin(factory);
	int listed = 0;
	for (int i = gpGlobals->maxClients + 1; i < gpGlobals->maxEntities && listed < 16; ++i)
	{
		edict_t* e = engine->PEntityOfEntIndex(i);
		if (e == nullptr || e->IsFree() || e == factory || e->GetCollideable() == nullptr)
			continue;
		ICollideable* c = e->GetCollideable();
		if (c->GetSolid() == SOLID_NONE || (c->GetSolidFlags() & (FSOLID_NOT_SOLID | FSOLID_TRIGGER)) != 0)
			continue;
		Vector mins, maxs;
		c->WorldSpaceSurroundingBounds(&mins, &maxs);
		if (maxs.x < fmins.x || mins.x > fmaxs.x || maxs.y < fmins.y || mins.y > fmaxs.y ||
			maxs.z < fmins.z || mins.z > fmaxs.z)
			continue;
		++listed;
		const Vector eo = CBotGlobals::entityOrigin(e);
		RCBOT_DIAG("[RCBOT2] vehicle factory-near factory=%d ent=%d class=%s group=%d dist=%.0f at=(%.0f %.0f %.0f)\n",
			factoryIndex, i, e->GetClassName() != nullptr ? e->GetClassName() : "?", c->GetCollisionGroup(),
			(eo - fo).Length(), eo.x, eo.y, eo.z);
	}
}

const char* StageName(EmpiresVehicleStage stage)
{
	switch (stage)
	{
	case EmpiresVehicleStage::NONE: return "none";
	case EmpiresVehicleStage::GOTO_FACTORY: return "goto-factory";
	case EmpiresVehicleStage::BUYING: return "buying";
	case EmpiresVehicleStage::WAIT_SPAWN: return "wait-spawn";
	case EmpiresVehicleStage::APPROACH: return "approach";
	case EmpiresVehicleStage::ENTERING: return "entering";
	case EmpiresVehicleStage::DRIVING: return "driving";
	case EmpiresVehicleStage::STUN_EXIT: return "stun-exit";
	case EmpiresVehicleStage::GIVE_UP_EXIT: return "give-up-exit";
	case EmpiresVehicleStage::REPAIR_ASSIST: return "repair-assist";
	case EmpiresVehicleStage::STUN_THROW: return "stun-throw";
	}
	return "?";
}

float NormalizeYaw(float yaw)
{
	while (yaw > 180.0f) yaw -= 360.0f;
	while (yaw < -180.0f) yaw += 360.0f;
	return yaw;
}

float YawTo(const Vector& from, const Vector& to)
{
	return std::atan2(to.y - from.y, to.x - from.x) * 180.0f / kPi;
}

Vector YawDir(float yaw)
{
	const float r = yaw * kPi / 180.0f;
	return Vector(std::cos(r), std::sin(r), 0.0f);
}

float Dist2D(const Vector& a, const Vector& b)
{
	return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y));
}

bool IsClass(const edict_t* e, const char* name)
{
	return e != nullptr && !e->IsFree() && e->GetClassName() != nullptr &&
		std::strcmp(e->GetClassName(), name) == 0;
}

// World ground under p within kRoadProbeDown of refZ. World only: the road is
// world geometry, everything below it is void (rainbowroad) or a cliff drop.
// Ford depth of the vehicle being steered (set in tickEmpiresVehicle), 0 = no water check.
float g_driveWaterDepth = 0.0f;
struct DriveWaterDepthScope
{
	explicit DriveWaterDepthScope(float depth) { g_driveWaterDepth = depth; }
	~DriveWaterDepthScope() { g_driveWaterDepth = 0.0f; }
};

bool GroundAt(const Vector& p, float refZ)
{
	if (enginetrace == nullptr)
		return true;
	Ray_t ray;
	ray.Init(Vector(p.x, p.y, refZ + kRoadProbeUp), Vector(p.x, p.y, refZ - kRoadProbeDown));
	CTraceFilterWorldOnly filter;
	trace_t hit;
	enginetrace->TraceRay(ray, MASK_PLAYERSOLID_BRUSHONLY, &filter, &hit);
	if (hit.fraction >= 1.0f || hit.allsolid)
		return false;
	// Sea floor is ground for the trace, but a body centre under water drowns the vehicle
	// (20 HP/s, engine off). g_driveWaterDepth is the driven vehicle's ford depth.
	if (g_driveWaterDepth > 0.0f &&
		(enginetrace->GetPointContents(hit.endpos + Vector(0, 0, g_driveWaterDepth)) & MASK_WATER) != 0)
		return false;
	return true;
}

// trigger_push volumes (rainbowroad boost pads push with 700000). Against the
// push they are an invisible wall: both test vehicles stuck at the box edge.
struct PushZone
{
	edict_t* edict;
	Vector mins;
	Vector maxs;
	Vector dir; // horizontal world push direction
};
std::vector<PushZone> g_pushZones;
float g_pushZonesAt = -1.0f;

void RefreshPushZones()
{
	const float now = gpGlobals->curtime;
	if (g_pushZonesAt >= 0.0f && now >= g_pushZonesAt && now - g_pushZonesAt < 5.0f)
		return;
	g_pushZonesAt = now;
	g_pushZones.clear();
	for (int i = gpGlobals->maxClients + 1; i < gpGlobals->maxEntities; ++i)
	{
		edict_t* e = engine->PEntityOfEntIndex(i);
		if (!IsClass(e, "trigger_push") || e->GetCollideable() == nullptr)
			continue;
		IServerUnknown* unknown = e->GetUnknown();
		CBaseEntity* entity = unknown != nullptr ? unknown->GetBaseEntity() : nullptr;
		const datamap_t* map = entity != nullptr ? CBaseEntity_GetDataDescMap(entity) : nullptr;
		const unsigned dirOffset = map != nullptr ? UTIL_FindInDataMap(map, "m_vecPushDir") : 0;
		const unsigned offOffset = map != nullptr ? UTIL_FindInDataMap(map, "m_bDisabled") : 0;
		if (dirOffset == 0)
			continue;
		if (offOffset != 0 && *reinterpret_cast<const bool*>(reinterpret_cast<const char*>(entity) + offOffset))
			continue;
		// Activate() stores the direction in entity space.
		const Vector local = *reinterpret_cast<const Vector*>(reinterpret_cast<const char*>(entity) + dirOffset);
		matrix3x4_t toWorld;
		AngleMatrix(e->GetCollideable()->GetCollisionAngles(), toWorld);
		Vector world;
		VectorRotate(local, toWorld, world);
		world.z = 0.0f;
		if (world.NormalizeInPlace() < 0.2f)
			continue; // mostly vertical
		PushZone zone;
		zone.edict = e;
		e->GetCollideable()->WorldSpaceSurroundingBounds(&zone.mins, &zone.maxs);
		zone.dir = world;
		g_pushZones.push_back(zone);
	}
	static size_t s_logged = size_t(-1);
	if (s_logged != g_pushZones.size())
	{
		s_logged = g_pushZones.size();
		for (const PushZone& zone : g_pushZones)
			RCBOT_DIAG("[RCBOT2] vehicle push-zone idx=%d min=(%.0f %.0f %.0f) max=(%.0f %.0f %.0f) dir=(%.2f %.2f)\n",
				engine->IndexOfEdict(zone.edict), zone.mins.x, zone.mins.y, zone.mins.z,
				zone.maxs.x, zone.maxs.y, zone.maxs.z, zone.dir.x, zone.dir.y);
	}
}

// True when p lies in a horizontal push volume.
bool PushAgainst(const Vector& p, float yaw)
{
	(void)yaw; // either direction is avoided, see below
	for (const PushZone& zone : g_pushZones)
	{
		if (p.x < zone.mins.x || p.x > zone.maxs.x || p.y < zone.mins.y || p.y > zone.maxs.y ||
			p.z < zone.mins.z - 150.0f || p.z > zone.maxs.z)
			continue;
		// Against the push it is a wall; with it the boost (rainbowroad 700000)
		// spins the vehicle at ~1700 u/s (run 201308). Avoid both.
		return true;
	}
	return false;
}

// Ground along a heading: centre line at half and full look-ahead plus both
// sides of the vehicle at full look-ahead; no opposing push volume ahead.
bool SafeHeading(const Vector& origin, float yaw, float lookAhead)
{
	const Vector dir = YawDir(yaw);
	const Vector side(-dir.y, dir.x, 0.0f);
	const float refZ = origin.z;
	const float pushLook = (std::max)(lookAhead, 350.0f);
	if (PushAgainst(origin + dir * pushLook, yaw) ||
		PushAgainst(origin + dir * pushLook + side * 100.0f, yaw) ||
		PushAgainst(origin + dir * pushLook - side * 100.0f, yaw))
		return false;
	return GroundAt(origin + dir * (lookAhead * 0.5f), refZ) &&
		GroundAt(origin + dir * lookAhead, refZ) &&
		GroundAt(origin + dir * lookAhead + side * 80.0f, refZ) &&
		GroundAt(origin + dir * lookAhead - side * 80.0f, refZ);
}

// Distance from p to the nearest drop in direction yaw (searched up to range).
float DropDistance(const Vector& p, float yaw, float range)
{
	const Vector dir = YawDir(yaw);
	for (float d = 150.0f; d <= range; d += 150.0f)
		if (!GroundAt(p + dir * d, p.z))
			return d;
	return range * 2.0f;
}

// World-only sight line a vehicle body can follow (two traces at its half width).
// Sight for vehicle targets: world and buildings block (a world-only trace let tanks in the
// enemy base shoot at infantry inside and behind barracks, human test 15.11.). Players and
// vehicles do not block; the physbox only fills a building's interior for vehicles.
class CVehicleSightFilter : public CTraceFilter
{
public:
	explicit CVehicleSightFilter(const edict_t* ignored = nullptr, bool entitiesOnly = false)
		: m_ignored(ignored), m_entitiesOnly(entitiesOnly) {}

	// Entities only: the navmesh route already follows the terrain, only buildings are unknown to it.
	TraceType_t GetTraceType() const override { return m_entitiesOnly ? TRACE_ENTITIES_ONLY : TRACE_EVERYTHING; }

	bool ShouldHitEntity(IHandleEntity* handle, int) override
	{
		if (handle == nullptr)
			return false;
		if (staticpropmgr != nullptr && staticpropmgr->IsStaticProp(handle))
			return !m_entitiesOnly;
		IServerUnknown* unknown = static_cast<IServerUnknown*>(handle);
		IServerNetworkable* networkable = unknown != nullptr ? unknown->GetNetworkable() : nullptr;
		edict_t* e = networkable != nullptr ? networkable->GetEdict() : nullptr;
		const char* cls = e != nullptr ? e->GetClassName() : nullptr;
		if (cls == nullptr || e == m_ignored)
			return false;
		if (std::strcmp(cls, "prop_dynamic") == 0 ||
			std::strcmp(cls, "func_button") == 0 ||
			std::strcmp(cls, "func_brush") == 0 ||
			std::strcmp(cls, "func_wall") == 0)
		{
			ICollideable* col = e->GetCollideable();
			return col != nullptr && col->GetSolid() != SOLID_NONE &&
				(col->GetSolidFlags() & (FSOLID_NOT_SOLID | FSOLID_TRIGGER)) == 0;
		}
		if (std::strcmp(cls, "emp_eng_map_model") == 0)
			return RCBotNavMesh::IsBuiltEmpiresMapObject(e);
		return std::strncmp(cls, "emp_building_", 13) == 0 ||
			std::strcmp(cls, "emp_nf_commander") == 0 || std::strcmp(cls, "emp_imp_commander") == 0;
	}

private:
	const edict_t* m_ignored;
	bool m_entitiesOnly;
};

constexpr float kVehicleMgInfantryRange = 1200.0f; // MG round 1000 u/s (vehicle_weapons.txt)

// 9.8: the context-steering traces run level, so a rising drivable slope (normal z >= 0.7) beside or ahead of
// the route read as a wall: on Palmbay the CV was steered away from the uphill route, down the shore bank at
// 585 u/s (runs 20261127-190553/-190533), and the side traces reported a passage on open ground.
float CtxWallFraction(const trace_t& hit)
{
	if (rcbot_empires_vehicle_ctx_ground_slopes.GetBool() && !hit.startsolid && hit.fraction < 1.0f &&
		hit.plane.normal.z >= 0.7f)
		return 1.0f;
	return hit.fraction;
}

// Consistent chassis-relative trace height (~42 HU above the bottom of the vehicle OBB).
// APC/Light/AFV origins sit ~40 HU above the ground (OBBMins.z ~ -36), so a fixed +48 trace ran
// ~88 HU above ground and shot straight over low solid prop_dynamic obstacles (crossroads 20261127-162205
// BE APC veh=563 hung on prop_dynamic #221 at z=-240).
float VehicleTraceLift(const edict_t* vehicle)
{
	edict_t* edict = const_cast<edict_t*>(vehicle);
	if (edict != nullptr && !edict->IsFree() && edict->GetCollideable() != nullptr)
		return (std::max)(8.0f, (std::min)(48.0f, edict->GetCollideable()->OBBMins().z + 42.0f));
	return 42.0f;
}

bool DriveLineClear(const Vector& from, const Vector& to, float halfWidth, const edict_t* drivenCV = nullptr,
	trace_t* blockedBy = nullptr, bool buildingsOnly = false)
{
	Vector dir = to - from;
	dir.z = 0.0f;
	const float len = dir.Length();
	if (len < 1.0f)
		return true;
	dir /= len;
	const Vector side(-dir.y, dir.x, 0.0f);
	const float fromLift = VehicleTraceLift(drivenCV);
	constexpr float kGoalLift = 42.0f; // route points sit on the navmesh floor
	CTraceFilterWorldOnly worldOnly;
	CVehicleSightFilter buildings(drivenCV, buildingsOnly);
	ITraceFilter* filter = drivenCV != nullptr ? static_cast<ITraceFilter*>(&buildings) : &worldOnly;
	for (const float s : {-1.0f, 1.0f})
	{
		const Vector a = from + side * (s * halfWidth) + Vector(0, 0, fromLift);
		const Vector b = to + side * (s * halfWidth) + Vector(0, 0, kGoalLift);
		trace_t hit;
		Ray_t ray;
		ray.Init(a, b);
		enginetrace->TraceRay(ray, MASK_PLAYERSOLID, filter, &hit);
		if (hit.fraction < 1.0f)
		{
			if (blockedBy != nullptr)
				*blockedBy = hit;
			return false;
		}
	}
	return true;
}

// Full-lock turn response from the turn-test samples (20261119-103539/-103623, 1x,
// full side input for 0.5 s): yaw starts after ~0.25 s and peaks at ~85 deg/s at
// ~680 HU/s (NF Light) and ~67 deg/s at ~725 HU/s (BE AFV), i.e. about 1000 and
// 850 HU/s^2 lateral. Per-chassis profiles live in EmpiresContextSteer.
struct VehicleTurnModel
{
	float lateralAccel;
	float minRadius;
};

VehicleTurnModel TurnModel(int chassis)
{
	const EmpiresContextSteer::TurnProfile p = EmpiresContextSteer::ProfileForChassis(chassis);
	return {p.lateralAccel, p.minRadius};
}

// Radius of a full-lock arc at this speed; side input s gives radius / |s|.
float TurnRadius(int chassis, float speed)
{
	return EmpiresContextSteer::TurnRadiusFor(chassis, speed);
}

float RouteCurveSpeed(const Vector& origin, const std::vector<Vector>& route, std::size_t first, int chassis)
{
	if (first >= route.size())
		return 0.0f;
	constexpr std::size_t kMaxPoints = 32;
	EmpiresContextSteer::Point2D pts[kMaxPoints];
	std::size_t count = 0;
	float dist = 0.0f;
	Vector prev = origin;
	for (std::size_t i = first; i < route.size() && count < kMaxPoints; ++i)
	{
		pts[count++] = {route[i].x, route[i].y};
		dist += Dist2D(prev, route[i]);
		prev = route[i];
		if (dist > 1900.0f)
			break;
	}
	return EmpiresContextSteer::PolylineCurveSpeed({origin.x, origin.y}, pts, count, 0,
		EmpiresContextSteer::ProfileForChassis(chassis));
}

// A damaged vehicle's call for engineers (emp_menu_command 30), one per team.
struct RepairRequest
{
	int vehicle = -1;
	int driverUserId = -1;
	Vector origin = Vector(0, 0, 0);
	float until = 0.0f;
	int engineerUserId = -1;
	float claimUntil = 0.0f;
	int side = -1; // weakest armor side: 0 front, 1 rear, 2 left, 3 right
	bool armorDamaged = false;
	bool baseScan = false; // found by an EngiBot in base / on a repair pad (e.g. human vehicle)
	bool human = false;    // TODO 8.3: a human drives it; bot calls do not take the slot over
};
RepairRequest s_repairRequest[4];
// TODO 8.4: the engineer walking with a tank (one per team).
struct EscortClaim
{
	int engineerUserId = -1;
	int vehicle = -1;
	float until = 0.0f;
};
EscortClaim s_escortClaim[4];
EmpiresAmmoRequest s_ammoRequest[4][kEmpiresAmmoRequestSlots];

} // namespace

// Hull plus all armor sides; used to see whether a rocket hit (bot_empires_skirmish.cpp).
bool EmpiresVehicleDamageTotal(const edict_t* e, float& out)
{
	if (!ReadVehicleHealth(e, out))
		return false;
	for (int k = 0; k < 4; ++k)
		out += static_cast<float>((std::max)(0, ReadArmorSide(e, k)));
	return true;
}

EmpiresAmmoRequest* EmpiresAmmoRequests(int team)
{
	return s_ammoRequest[team >= 0 && team <= 3 ? team : 0];
}

EmpiresAmmoRequest* EmpiresAmmoRequestForVehicle(int team, int vehicle, bool create)
{
	if (vehicle <= 0 || gpGlobals == nullptr)
		return nullptr;
	const float now = gpGlobals->curtime;
	EmpiresAmmoRequest* slots = EmpiresAmmoRequests(team);
	auto live = [now](const EmpiresAmmoRequest& r) { return r.vehicle > 0 && now <= r.until; };
	auto held = [now](const EmpiresAmmoRequest& r) { return r.engineerUserId >= 0 && now <= r.claimUntil; };
	auto rank = [&](const EmpiresAmmoRequest& r) { return !live(r) ? 0 : !held(r) ? 1 : 2; };
	EmpiresAmmoRequest* spare = nullptr;
	for (int i = 0; i < kEmpiresAmmoRequestSlots; ++i)
	{
		EmpiresAmmoRequest& r = slots[i];
		if (live(r) && r.vehicle == vehicle)
			return &r;
		// Prefer a dead slot, then a live one no engineer holds, each the one that runs out first.
		if (spare == nullptr || rank(r) < rank(*spare) || (rank(r) == rank(*spare) && r.until < spare->until))
			spare = &r;
	}
	if (!create || spare == nullptr)
		return nullptr;
	*spare = EmpiresAmmoRequest();
	spare->vehicle = vehicle;
	return spare;
}

void EmpiresAmmoRequestClear(int team, int vehicle)
{
	EmpiresAmmoRequest* slots = EmpiresAmmoRequests(team);
	for (int i = 0; i < kEmpiresAmmoRequestSlots; ++i)
		if (vehicle > 0 && slots[i].vehicle == vehicle)
			slots[i] = EmpiresAmmoRequest();
}

namespace
{
edict_t* RepairRequestVehicle(int team)
{
	if (team < 0 || team > 3 || gpGlobals == nullptr)
		return nullptr;
	RepairRequest& r = s_repairRequest[team];
	if (r.vehicle <= 0 || gpGlobals->curtime > r.until)
		return nullptr;
	edict_t* e = engine->PEntityOfEntIndex(r.vehicle);
	if (e == nullptr || e->IsFree() || !IsClass(e, "emp_vehicle"))
		return nullptr;
	return e;
}

// Point of the vehicle's oriented box nearest to p.
Vector NearestOnVehicle(edict_t* vehicle, const Vector& p)
{
	ICollideable* c = vehicle->GetCollideable();
	if (c == nullptr)
		return CBotGlobals::entityOrigin(vehicle);
	matrix3x4_t toWorld;
	AngleMatrix(c->GetCollisionAngles(), c->GetCollisionOrigin(), toWorld);
	Vector local;
	VectorITransform(p, toWorld, local);
	const Vector mins = c->OBBMins(), maxs = c->OBBMaxs();
	for (int i = 0; i < 3; ++i)
		local[i] = (std::max)(mins[i], (std::min)(maxs[i], local[i]));
	Vector out;
	VectorTransform(local, toWorld, out);
	return out;
}

} // namespace

int EmpiresVehicleReadInt(const CBotEmpires* bot, const edict_t* e, const char* prop, int fallback)
{
	int value = fallback;
	if (!bot->readEmpiresEntityIntProperty(e, prop, value))
		return fallback;
	return value;
}

// Which fire button reaches which loaded weapon in the turret's active group. Weapon IDs are
// the order in scripts/vehicle_weapons.txt: 0-11 machine guns (type 0; IDs 1, 4, 5, 8-11 are
// armour piercing or explosive), 12-22 cannons, 23-26 artillery, 27-39 missiles, 40-42 grenade
// launchers; 255 = empty entry. A button fires every weapon of the active group with its mode:
// IN_ATTACK mode 1, IN_ATTACK2 mode 0 (static RE; run 144912: bot-bought MG mode 1, cannon mode 0,
// matching the user's observation that IN_ATTACK fires the MG). Artillery is never chosen.
static void ReadVehicleWeapons(const CBotEmpires* bot, edict_t* turret, EmpiresVehicleBrain& v)
{
	v.weaponsKnown = false;
	v.weaponMgPure = v.weaponMgMixed = v.weaponHeavyInf = v.weaponArmour = 0;
	v.weaponMgLeft = false;
	v.weaponHeavyPresent = v.weaponHeavyLeft = false;
	v.weaponCannonLeft = v.weaponMissileLeft = false;
	v.weaponActiveCannonLoaded = v.weaponActiveMissileLoaded = false;
	v.weaponHeavyTotal = 0;
	v.weaponSummary[0] = '\0';
	std::int32_t weapons[8], ammo[8], clips[8];
	std::uint8_t modes[8], groups[48];
	if (turret == nullptr || !ReadTurretArray(turret, "m_iWeapon", weapons) ||
		!ReadTurretArray(turret, "m_iWeaponAmmo", ammo) || !ReadTurretArray(turret, "m_iWeaponClips", clips) ||
		!ReadTurretArray(turret, "m_bWeaponMode", modes) || !ReadTurretArray(turret, "m_bWeaponGroup", groups))
		return;
	const int group = EmpiresVehicleReadInt(bot, turret, "m_iGroup", -1);
	// 3.10f: fire masks of the active group, ammo across all groups (standing in group 2 hid the
	// group-1 missiles and could start an ammo trip with missiles left).
	const EmpiresVehicleAim::TurretWeaponSummary sum =
		EmpiresVehicleAim::SummarizeTurretWeapons(weapons, ammo, clips, modes, groups, group, IN_ATTACK, IN_ATTACK2);
	if (!sum.valid)
		return;
	std::size_t used = 0;
	for (int i = 0; i < 8; ++i)
	{
		const int id = weapons[i];
		if (id < 0 || id > 42 || !groups[i * 6 + group] || used >= sizeof(v.weaponSummary))
			continue;
		const int n = snprintf(v.weaponSummary + used, sizeof(v.weaponSummary) - used, "%s%d:%s:%d/%d",
			used ? "," : "", id, modes[i] ? "a1" : "a2", ammo[i], clips[i]);
		if (n > 0)
			used += static_cast<std::size_t>(n);
	}
	v.weaponMgPure = sum.mgPure;
	v.weaponMgMixed = sum.mgMixed;
	v.weaponHeavyInf = sum.heavyInf;
	v.weaponArmour = sum.armour;
	v.weaponMgLeft = sum.activeMgLeft;
	v.weaponHeavyPresent = sum.heavyPresent;
	v.weaponHeavyLeft = sum.heavyLeft;
	v.weaponHeavyTotal = sum.heavyTotal;
	v.weaponCannonLeft = sum.cannonLeft;
	v.weaponMissileLeft = sum.missileLeft;
	v.weaponActiveCannonLoaded = sum.activeCannonLoaded;
	v.weaponActiveMissileLoaded = sum.activeMissileLoaded;
	v.weaponGroup = group;
	v.weaponGroupCount = sum.weaponGroupCount;
	v.weaponsKnown = true;
}

// 3.10f: the vehicle carries a missile launcher (weapon IDs 27-39) in any group.
static bool VehicleCarriesMissile(edict_t* turret)
{
	std::int32_t weapons[8];
	if (turret == nullptr || !ReadTurretArray(turret, "m_iWeapon", weapons))
		return false;
	for (int i = 0; i < 8; ++i)
	{
		if (weapons[i] >= 27 && weapons[i] <= 39)
			return true;
	}
	return false;
}

int EmpiresVehiclePlayerVehicle(const CBotEmpires* bot, const edict_t* player)
{
	int index = -1;
	if (!bot->readEmpiresWeaponHandleProperty(player, "m_hVehicle", index))
		return -1;
	// An empty handle reads as entry 4095 (INVALID_EHANDLE low bits).
	if (index <= 0 || gpGlobals == nullptr || index >= gpGlobals->maxEntities)
		return -1;
	return index;
}

namespace
{
bool IsCommanderChassis(int chassis)
{
	return chassis >= 14 && chassis <= 16;
}

bool IsApcChassis(int chassis)
{
	return chassis == 1 || chassis == 9;
}

} // namespace

bool EmpiresBotWantsVfSpawn(const CBotEmpires* bot, int team)
{
	if (bot == nullptr || bot->getEdict() == nullptr || !rcbot_empires_vehicles.GetBool())
		return false;
	const int client = engine != nullptr ? engine->IndexOfEdict(bot->getEdict()) : 0;
	if (client > 0 && EmpiresLawlietWantsApcBuy(team, client))
		return true;
	if (client > 0 && EmpiresLawlietBlocksVehicleEntry(team, client))
		return false;
	float pool = -1.0f;
	EmpiresTeamResources(team, pool);
	const float wages = EmpiresPlayerWages(bot->getEdict());
	const EmpiresTeamEconomy::TeamState& money = EmpiresTeamEconomy::Team(team);
	const float now = gpGlobals != nullptr ? gpGlobals->curtime : 0.0f;
	const float reserve = EmpiresTeamEconomy::ActiveReserve(money, now);
	if (!EmpiresTeamEconomy::PurchaseAllowed(wages, pool, money.vehicleCost, reserve))
		return false;

	// Same driver quota as the vehicle buy path (TODO 5.24): without it every
	// non-Engineer spawned at the VF as soon as the team could afford one tank
	// (run 20261121-130658). Recent VF-spawn claims of bots that are not driving
	// yet count as drivers, so simultaneous respawns do not all pick the VF.
	constexpr float kClaimFor = 30.0f;
	static float s_vfSpawnClaim[RCBOT_MAXPLAYERS] = {};
	int drivers = 0;
	int teamBots = 1;
	for (int slot = 0; slot < RCBOT_MAXPLAYERS; ++slot)
	{
		CBot* other = CBots::getBot(slot);
		if (other == nullptr || other == bot || !other->inUse() || other->getEdict() == nullptr ||
			CClassInterface::getTeam(other->getEdict()) != team)
			continue;
		const CBotEmpires* empOther = static_cast<const CBotEmpires*>(other);
		++teamBots;
		if (empOther->m_vehicle.stage != EmpiresVehicleStage::NONE && !empOther->m_vehicle.billNye)
			++drivers;
		else if (s_vfSpawnClaim[slot] > 0.0f && now - s_vfSpawnClaim[slot] < kClaimFor)
			++drivers;
	}
	const int limit = rcbot_empires_vehicle_drivers.GetInt() >= 0 ? rcbot_empires_vehicle_drivers.GetInt() :
		(std::max)(2, (teamBots * 2 + 2) / 5);
	if (drivers >= limit)
		return false;
	for (int slot = 0; slot < RCBOT_MAXPLAYERS; ++slot)
		if (CBots::getBot(slot) == bot)
			s_vfSpawnClaim[slot] = now;
	return true;
}

bool EmpiresVehicleExceedsSpeed(edict_t* vehicle, float limit)
{
	if (vehicle == nullptr || vehicle->IsFree() || vehicle->GetUnknown() == nullptr)
		return false;
	CBaseEntity* entity = vehicle->GetUnknown()->GetBaseEntity();
	const datamap_t* map = entity != nullptr ? CBaseEntity_GetDataDescMap(entity) : nullptr;
	const unsigned offset = UTIL_FindInDataMap(map, "m_vecVelocity");
	if (offset == 0)
		return false;
	const Vector& velocity = *reinterpret_cast<const Vector*>(reinterpret_cast<const char*>(entity) + offset);
	return velocity.Length2D() > limit;
}

bool EmpiresVehicleAlive(const CBotEmpires* bot, const edict_t* vehicle)
{
	if (vehicle == nullptr || vehicle->IsFree())
		return false;
	float health = 1.0f;
	if (ReadEntityFloat(vehicle, "m_flTotalHealth", health) && health <= 0.0f)
		return false;
	return true;
}

namespace
{
// Hits world and entities, never players (they move out of the way).
class CTraceFilterNoPlayers : public CTraceFilter
{
public:
	bool ShouldHitEntity(IHandleEntity* handle, int) override
	{
		IServerUnknown* unknown = static_cast<IServerUnknown*>(handle);
		IServerNetworkable* networkable = unknown != nullptr ? unknown->GetNetworkable() : nullptr;
		edict_t* edict = networkable != nullptr ? networkable->GetEdict() : nullptr;
		if (edict == nullptr || gpGlobals == nullptr)
			return true;
		const int index = engine->IndexOfEdict(edict);
		return index <= 0 || index > gpGlobals->maxClients;
	}
};

// Fire line: everything solid except the shooter, his vehicle and players
// (they step aside); the target itself counts as a hit.
class FireLineFilter : public CTraceFilter
{
public:
	FireLineFilter(edict_t* self, edict_t* vehicle, edict_t* target) : m_self(self), m_vehicle(vehicle), m_target(target) {}
	bool ShouldHitEntity(IHandleEntity* handle, int) override
	{
		IServerUnknown* unknown = static_cast<IServerUnknown*>(handle);
		IServerNetworkable* networkable = unknown != nullptr ? unknown->GetNetworkable() : nullptr;
		edict_t* edict = networkable != nullptr ? networkable->GetEdict() : nullptr;
		if (edict == nullptr || gpGlobals == nullptr)
			return true;
		if (edict == m_self || edict == m_vehicle)
			return false;
		const int index = engine->IndexOfEdict(edict);
		if (edict != m_target)
		{
			const char* cls = edict->GetClassName();
			// The game's ShouldCollide ignores resource points (RE engineer.md); the volume under a
			// refinery blocked every close shot at it (run 125722: blocker=emp_resource_point at 524 HU).
			// emp_eng_restrict is an invisible no-build brush that does not block projectiles
			// (run 20261121-105411: 20+ fire-blocked frac=0.00 between two tanks at 300 HU).
			if (cls != nullptr && (std::strcmp(cls, "emp_resource_point") == 0 ||
				std::strcmp(cls, "emp_eng_restrict") == 0))
				return false;
			if (cls != nullptr && std::strncmp(cls, "emp_building_", 13) == 0 &&
				(std::strstr(cls, "physbox") != nullptr || std::strstr(cls, "panel") != nullptr))
				return false;
			if (const ICollideable* col = edict->GetCollideable())
				if (col->GetCollisionGroup() == COLLISION_GROUP_VEHICLE_CLIP)
					return false;
			// Our own turret surrounds the eye; every shot started inside it
			// (run 20261120-201110: heavy BE tanks, 18x fire-blocked frac=0.00
			// by their own emp_tank_turret, refinery never shot).
			if (cls != nullptr && m_vehicle != nullptr && std::strcmp(cls, "emp_tank_turret") == 0 &&
				Dist2D(CBotGlobals::entityOrigin(edict), CBotGlobals::entityOrigin(m_vehicle)) < 300.0f)
				return false;
		}
		return edict == m_target || index <= 0 || index > gpGlobals->maxClients;
	}

private:
	edict_t* m_self;
	edict_t* m_vehicle;
	edict_t* m_target;
};

Vector EntityCenter(edict_t* e);

// why (optional): "world" / "entity" / "" plus fraction and blocking entity index for diagnosis.
// Diagnosis (rcbot_empires_vehicle_shell_trace): follows cannon shells/missiles and logs their
// last seen position when they disappear, with the distance to the target box.
void TraceShells(int userId, edict_t* target)
{
	struct Tracked { int index = -1; int serial = -1; Vector first; Vector last; const char* cls = ""; };
	static Tracked tracked[64];
	if (gpGlobals == nullptr || engine == nullptr)
		return;
	for (Tracked& t : tracked)
	{
		if (t.index < 0)
			continue;
		edict_t* e = engine->PEntityOfEntIndex(t.index);
		if (e != nullptr && !e->IsFree() && e->m_NetworkSerialNumber == t.serial)
		{
			t.last = CBotGlobals::entityOrigin(e);
			continue;
		}
		Vector mins(0, 0, 0), maxs(0, 0, 0);
		if (target != nullptr && !target->IsFree() && target->GetCollideable() != nullptr)
			target->GetCollideable()->WorldSpaceSurroundingBounds(&mins, &maxs);
		const float dx = (std::max)((std::max)(mins.x - t.last.x, t.last.x - maxs.x), 0.0f);
		const float dy = (std::max)((std::max)(mins.y - t.last.y, t.last.y - maxs.y), 0.0f);
		const float dz = (std::max)((std::max)(mins.z - t.last.z, t.last.z - maxs.z), 0.0f);
		const Vector flight = t.last - t.first;
		RCBOT_DIAG("[RCBOT2] vehicle shell-end userid=%d class=%s from=(%.0f %.0f %.0f) at=(%.0f %.0f %.0f) flight_yaw=%.1f "
			"target=%d target_yaw=%.1f box_dist=%.0f\n", userId, t.cls, t.first.x, t.first.y, t.first.z, t.last.x, t.last.y,
			t.last.z, RAD2DEG(std::atan2(flight.y, flight.x)), target != nullptr ? engine->IndexOfEdict(target) : -1,
			target != nullptr ? YawTo(t.first, EntityCenter(target)) : 0.0f, std::sqrt(dx * dx + dy * dy + dz * dz));
		t.index = -1;
	}
	for (int i = gpGlobals->maxClients + 1; i < gpGlobals->maxEntities; ++i)
	{
		edict_t* e = engine->PEntityOfEntIndex(i);
		if (e == nullptr || e->IsFree() || e->GetClassName() == nullptr)
			continue;
		const char* cls = e->GetClassName();
		if (std::strstr(cls, "shell") == nullptr && std::strstr(cls, "cannon") == nullptr &&
			std::strstr(cls, "missile") == nullptr)
			continue;
		bool known = false;
		Tracked* slot = nullptr;
		for (Tracked& t : tracked)
		{
			if (t.index == i && t.serial == e->m_NetworkSerialNumber)
				known = true;
			else if (t.index < 0 && slot == nullptr)
				slot = &t;
		}
		if (!known && slot != nullptr)
		{
			slot->index = i;
			slot->serial = e->m_NetworkSerialNumber;
			slot->last = CBotGlobals::entityOrigin(e);
			slot->first = slot->last;
			slot->cls = cls;
		}
	}
}

bool HasClearFireLine(edict_t* self, edict_t* vehicle, const Vector& eye, edict_t* target,
	const char** why = nullptr, float* fraction = nullptr, int* blocker = nullptr, Vector* end = nullptr)
{
	if (enginetrace == nullptr || target == nullptr || target->IsFree())
		return false;
	trace_t hit;
	Ray_t ray;
	const Vector centre = EntityCenter(target);
	if (end != nullptr)
		*end = centre;
	ray.Init(eye, centre);
	CTraceFilterWorldOnly worldFilter;
	enginetrace->TraceRay(ray, MASK_SHOT, &worldFilter, &hit);
	if (hit.fraction <= 0.97f)
	{
		if (why != nullptr)
			*why = "world";
		if (fraction != nullptr)
			*fraction = hit.fraction;
		return false;
	}
	FireLineFilter entities(self, vehicle, target);
	enginetrace->TraceRay(ray, MASK_SHOT, &entities, &hit);
	const bool clear = hit.fraction > 0.97f ||
		(target->GetUnknown() != nullptr && hit.m_pEnt == target->GetUnknown()->GetBaseEntity());
	if (!clear)
	{
		if (why != nullptr)
			*why = "entity";
		if (fraction != nullptr)
			*fraction = hit.fraction;
		if (blocker != nullptr && hit.m_pEnt != nullptr)
		{
			edict_t* e = servergameents != nullptr ? servergameents->BaseEntityToEdict(hit.m_pEnt) : nullptr;
			*blocker = e != nullptr ? engine->IndexOfEdict(e) : -1;
		}
	}
	return clear;
}

struct VehicleManeuver
{
	bool valid = false;
	float steer = 0.0f;
	float speed = 0.0f;
	int blocker = -1;
	// Diagnosis of the first hit on the route controller's own forward arc.
	float blockDistance = -1.0f;
	float blockNormalZ = 0.0f;
	Vector blockAt = Vector(0, 0, 0);
	int blockerGroup = -1;
	int blockerSolid = -1;
	int blockContents = 0;
	char blockSurface[48] = "-";
	int passableCandidate = -1; // entity the vehicle overlaps while rolling on
	bool braking = false; // no usable arc: brake for the route path's free distance
};

// Own entity rules: the shared CTraceFilterSimple/StandardFilterRules in
// navmesh/util/UtilTrace.cpp rejects every entity when the mask contains
// CONTENTS_MONSTER (MASK_SOLID does), so a planner using it saw only world
// geometry and drove into props and vehicles (obstacle runs 20261119-1304..1308).
class VehicleManeuverTraceFilter : public CTraceFilter
{
public:
	VehicleManeuverTraceFilter(const edict_t* driver, const edict_t* vehicle, const Vector& origin,
		const int* passable = nullptr)
		: m_driver(driver), m_vehicle(vehicle), m_origin(origin), m_passable(passable) {}

	bool ShouldHitEntity(IHandleEntity* handle, int contentsMask) override
	{
		if (handle == nullptr)
			return false;
		if (staticpropmgr != nullptr && staticpropmgr->IsStaticProp(handle))
			return true; // rocks, posts, crates baked into the map
		IServerUnknown* unknown = static_cast<IServerUnknown*>(handle);
		IServerNetworkable* networkable = unknown->GetNetworkable();
		edict_t* entity = networkable != nullptr ? networkable->GetEdict() : nullptr;
		if (entity == nullptr || entity == m_driver || entity == m_vehicle || entity->IsFree())
			return false;
		ICollideable* collide = entity->GetCollideable();
		if (collide == nullptr || collide->GetSolid() == SOLID_NONE ||
			(collide->GetSolidFlags() & (FSOLID_NOT_SOLID | FSOLID_TRIGGER)) != 0)
			return false;
		const int index = engine->IndexOfEdict(entity);
		if (gpGlobals != nullptr && index >= 1 && index <= gpGlobals->maxClients)
			return false; // infantry moves out of the way or is run over
		for (int k = 0; m_passable != nullptr && k < kManeuverPassableSlots; ++k)
			if (m_passable[k] == index)
				return false;
		const char* classname = entity->GetClassName();
		if (classname != nullptr)
		{
			if (std::strncmp(classname, "trigger_", 8) == 0 ||
				std::strncmp(classname, "emp_comm_restrict", 17) == 0 ||
				std::strncmp(classname, "emp_eng_restrict", 16) == 0 ||
				std::strcmp(classname, "emp_resource_point") == 0) // driven over at 483-505 HU/s (VF runs 1336, 1357)
				return false;
			if ((std::strcmp(classname, "emp_eng_map_brush") == 0 ||
				std::strcmp(classname, "emp_eng_map_model") == 0) &&
				!RCBotNavMesh::IsBuiltEmpiresMapObject(entity))
				return false;
			// Our own rotating gun sits on the hull; other vehicles' turrets
			// stand on a hull the trace hits anyway.
			if (std::strcmp(classname, "emp_tank_turret") == 0 &&
				Dist2D(CBotGlobals::entityOrigin(entity), m_origin) < 300.0f)
				return false;
		}
		return CGameRulesShouldCollide(COLLISION_GROUP_VEHICLE, collide->GetCollisionGroup());
	}

private:
	const edict_t* m_driver;
	const edict_t* m_vehicle;
	Vector m_origin;
	const int* m_passable;
};

// Sample the forward hull during the straight exit for diagnostics. The
// normal exit checks only ground and world clearance and keeps its controls.
void LogFactoryExitHull(int userId, edict_t* driver, edict_t* vehicle, const Vector& origin, float heading, float look,
	const char* label = "factory-exit")
{
	const ICollideable* hull = vehicle->GetCollideable();
	if (hull == nullptr || enginetrace == nullptr || servergameents == nullptr)
		return;
	const Vector mins = hull->OBBMins();
	const Vector maxs = hull->OBBMaxs();
	Ray_t ray;
	ray.Init(origin + Vector(0, 0, 80), origin - Vector(0, 0, 180));
	CTraceFilterWorldOnly world;
	trace_t floor;
	enginetrace->TraceRay(ray, MASK_PLAYERSOLID_BRUSHONLY, &world, &floor);
	if (floor.startsolid || floor.fraction >= 1.0f)
		return;
	const float bottom = (std::max)(mins.z, floor.endpos.z - origin.z + 30.0f);
	if (bottom >= maxs.z)
		return;
	// Use the same cross-section as the maneuver sweep. The hull faces local +Y.
	const Vector axis = YawDir(heading);
	const Vector side(axis.y, -axis.x, 0);
	const Vector center = origin + side * (0.5f * (mins.x + maxs.x));
	const float extent = (maxs.x - mins.x) * 0.5f / (std::fabs(axis.x) + std::fabs(axis.y));
	ray.Init(center + axis * (0.5f * (mins.y + maxs.y)), center + axis * (maxs.y + look),
		Vector(-extent, -extent, bottom), Vector(extent, extent, maxs.z));
	VehicleManeuverTraceFilter filter(driver, vehicle, origin);
	trace_t hit;
	enginetrace->TraceRay(ray, MASK_SOLID, &filter, &hit);
	edict_t* blocker = hit.m_pEnt != nullptr ? servergameents->BaseEntityToEdict(hit.m_pEnt) : nullptr;
	const bool hitWorld = blocker == nullptr || engine->IndexOfEdict(blocker) == 0;
	const Vector at = hitWorld ? hit.endpos : CBotGlobals::entityOrigin(blocker); // worldspawn origin is 0 0 0
	const char* classname = blocker != nullptr ? blocker->GetClassName() : nullptr;
	// surface "**studio**" = static prop (palm, rock); otherwise brush/displacement geometry.
	RCBOT_DIAG("[RCBOT2] vehicle %s hull userid=%d blocker=%d class=%s at=(%.0f %.0f %.0f) "
		"fraction=%.3f startsolid=%d normal_z=%.2f surface=%s\n", label, userId,
		blocker != nullptr ? engine->IndexOfEdict(blocker) : -1,
		classname != nullptr ? classname : (hit.DidHit() ? "geometry" : "none"), at.x, at.y, at.z,
		hit.fraction, hit.startsolid ? 1 : 0, hit.startsolid ? -2.0f : hit.plane.normal.z,
		hit.surface.name != nullptr ? hit.surface.name : "-");
}

// Planner cost, summed over all vehicles and logged every 10 s as
// "vehicle maneuver-perf" while the local maneuver selection is enabled.
struct ManeuverPerf
{
	int calls = 0;
	int deferred = 0;
	int traces = 0;
	double seconds = 0.0;
	double worst = 0.0;
	float logAt = 0.0f;
};
ManeuverPerf g_maneuverPerf;
int g_maneuverTick = -1;
int g_maneuverTickCalls = 0;

// Test-only obstacle on a driver's route (rcbotd empires_vehicle_obstacle). The
// command arms it; the first driver of NF Light/BE AFV with a long enough route
// ahead and at least entrySpeed gets the prop placed `ahead` along its route.
struct VehicleObstacleTest
{
	bool armed = false;
	bool active = false;
	float ahead = 700.0f;
	float lateral = 0.0f;
	float entrySpeed = 450.0f;
	char model[128] = "models/props/concretebarrier.mdl";
	int userId = -1;
	int prop = -1;
	Vector at = Vector(0, 0, 0);
	Vector dir = Vector(1, 0, 0);
	float depth = 0.0f; // half extent of the prop along the route
	float placedAt = 0.0f;
	float lastSample = 0.0f;
	float minDistance = 1.0e9f;
	float stoppedNear = 0.0f;
	float maneuverSeconds = 0.0f;
	int reversesAtStart = 0;
	float entry = 0.0f;
};
VehicleObstacleTest g_obstacleTest;

// Test-only (rcbotd empires_test_exit_block <userid>, TODO 5.29): box the bot's vehicle in
// with solid barriers around its hull so every exit point is blocked, then make it get out.
int ExitBlockSurround(edict_t* vehicle)
{
	ICollideable* hull = vehicle != nullptr ? vehicle->GetCollideable() : nullptr;
	if (hull == nullptr || servertools == nullptr || servergameents == nullptr)
		return 0;
	static const char* kModel = "models/props/concretebarrier.mdl"; // 224 x 26 x 54
	engine->PrecacheModel(kModel, true);
	const matrix3x4_t& toWorld = hull->CollisionToWorldTransform();
	const Vector mins = hull->OBBMins();
	const Vector maxs = hull->OBBMaxs();
	int placed = 0;
	for (const float margin : {20.0f, 60.0f}) // exit points lie ~35 outside the hull (run 144340)
	{
		const float x0 = mins.x - margin, x1 = maxs.x + margin, y0 = mins.y - margin, y1 = maxs.y + margin;
		const Vector corners[5] = {Vector(x0, y0, mins.z), Vector(x1, y0, mins.z), Vector(x1, y1, mins.z),
			Vector(x0, y1, mins.z), Vector(x0, y0, mins.z)};
		for (int edge = 0; edge < 4; ++edge)
		{
			Vector a, b;
			VectorTransform(corners[edge], toWorld, a);
			VectorTransform(corners[edge + 1], toWorld, b);
			const float length = Dist2D(a, b);
			const int count = (std::max)(1, static_cast<int>(std::ceil(length / 200.0f)));
			char angles[64];
			snprintf(angles, sizeof(angles), "0 %.1f 0", YawTo(a, b));
			for (int i = 0; i < count; ++i)
			{
				for (int layer = 0; layer < 3; ++layer)
				{
					Vector at = a + (b - a) * ((i + 0.5f) / count);
					at.z = a.z + 54.0f * layer;
					CBaseEntity* entity = servertools->CreateEntityByName("prop_dynamic");
					if (entity == nullptr)
						continue;
					servertools->SetKeyValue(entity, "model", kModel);
					servertools->SetKeyValue(entity, "solid", "6");
					servertools->SetKeyValue(entity, "angles", angles);
					servertools->SetKeyValue(entity, "origin", at);
					servertools->DispatchSpawn(entity);
					++placed;
				}
			}
		}
	}
	return placed;
}

// Places the armed obstacle `ahead` along the driver's remaining route and
// then reports how the driver gets past it: "vehicle obstacle placed",
// then "passed", "timeout" (45 s) or "abort" (driver left the vehicle).
void ObstacleTestTick(EmpiresVehicleBrain& v, int userId, edict_t* vehicle, int chassis, const Vector& origin,
	float speed, float forwardSpeed, float now)
{
	VehicleObstacleTest& t = g_obstacleTest;
	if (t.armed && !t.active && (chassis == 2 || chassis == 10) && !v.commanderCV && v.route.size() >= 2 &&
		v.routeIndex < v.route.size() && forwardSpeed >= t.entrySpeed && servertools != nullptr)
	{
		// Walk the route from the vehicle to the point `ahead`.
		Vector from = origin;
		float left = t.ahead;
		bool found = false;
		Vector at = origin;
		Vector dir = Vector(1, 0, 0);
		for (std::size_t k = v.routeIndex; k < v.route.size(); ++k)
		{
			Vector seg = v.route[k] - from;
			seg.z = 0.0f;
			const float length = seg.Length2D();
			if (length > 1.0f && length >= left)
			{
				dir = seg / length;
				at = from + dir * left;
				// Room behind the obstacle so "passed" can trigger on this route.
				float after = length - left;
				for (std::size_t m = k + 1; m < v.route.size(); ++m)
					after += Dist2D(v.route[m - 1], v.route[m]);
				found = after >= 400.0f;
				break;
			}
			left -= length;
			from = v.route[k];
		}
		if (!found)
			return; // try again on a later tick with a longer route ahead
		at += Vector(-dir.y, dir.x, 0.0f) * t.lateral;
		Ray_t ray;
		ray.Init(at + Vector(0, 0, 120), at - Vector(0, 0, 400));
		CTraceFilterWorldOnly world;
		trace_t hit;
		enginetrace->TraceRay(ray, MASK_PLAYERSOLID_BRUSHONLY, &world, &hit);
		if (hit.fraction >= 1.0f || hit.startsolid)
			return;
		at = hit.endpos;
		t.armed = false;
		engine->PrecacheModel(t.model, true);
		CBaseEntity* entity = servertools->CreateEntityByName("prop_dynamic");
		if (entity == nullptr)
		{
			RCBOT_DIAG("[RCBOT2] vehicle obstacle abort userid=%d reason=create-failed\n", userId);
			return;
		}
		char angles[64];
		snprintf(angles, sizeof(angles), "0 %.1f 0", YawTo(Vector(0, 0, 0), dir) + 90.0f);
		servertools->SetKeyValue(entity, "model", t.model);
		servertools->SetKeyValue(entity, "solid", "6");
		servertools->SetKeyValue(entity, "angles", angles);
		servertools->SetKeyValue(entity, "origin", at);
		servertools->DispatchSpawn(entity);
		edict_t* prop = servergameents != nullptr ? servergameents->BaseEntityToEdict(entity) : nullptr;
		Vector mins(0, 0, 0);
		Vector maxs(0, 0, 0);
		int solid = -1;
		float depth = 0.0f;
		if (prop != nullptr && prop->GetCollideable() != nullptr)
		{
			ICollideable* c = prop->GetCollideable();
			mins = c->OBBMins();
			maxs = c->OBBMaxs();
			solid = static_cast<int>(c->GetSolid());
			// Yaw = route + 90: the model's local Y lies along the route.
			depth = (std::max)(std::fabs(mins.y), std::fabs(maxs.y));
		}
		t.active = true;
		t.userId = userId;
		t.prop = prop != nullptr ? engine->IndexOfEdict(prop) : -1;
		t.at = at;
		t.dir = dir;
		t.depth = depth;
		t.placedAt = now;
		t.lastSample = now;
		t.minDistance = 1.0e9f;
		t.stoppedNear = 0.0f;
		t.maneuverSeconds = 0.0f;
		t.reversesAtStart = v.stuckReverses;
		t.entry = forwardSpeed;
		RCBOT_DIAG("[RCBOT2] vehicle obstacle placed userid=%d chassis=%d prop=%d model=%s solid=%d at=(%.0f %.0f %.0f) "
			"route_yaw=%.0f ahead=%.0f lateral=%.0f depth=%.0f mins=(%.0f %.0f %.0f) maxs=(%.0f %.0f %.0f) "
			"veh=(%.0f %.0f %.0f) entry=%.0f maneuvers=%d tick=%d\n", userId, chassis, t.prop, t.model, solid,
			at.x, at.y, at.z, YawTo(Vector(0, 0, 0), dir), t.ahead, t.lateral, depth, mins.x, mins.y, mins.z,
			maxs.x, maxs.y, maxs.z, origin.x, origin.y, origin.z, forwardSpeed,
			rcbot_empires_vehicle_maneuvers.GetBool() ? 1 : 0, gpGlobals->tickcount);
		return;
	}
	if (!t.active || t.userId != userId)
		return;
	const float step = (std::min)(0.5f, (std::max)(0.0f, now - t.lastSample));
	t.lastSample = now;
	const float distance = Dist2D(origin, t.at);
	t.minDistance = (std::min)(t.minDistance, distance);
	if (distance < 600.0f && speed < 40.0f)
		t.stoppedNear += step;
	if (rcbot_empires_vehicle_maneuvers.GetBool() && v.maneuverValid && now >= v.reverseUntil)
		t.maneuverSeconds += step;
	const ICollideable* hull = vehicle->GetCollideable();
	const float halfLength = hull != nullptr ? (hull->OBBMaxs().y - hull->OBBMins().y) * 0.5f : 150.0f;
	Vector offset = origin - t.at;
	offset.z = 0.0f;
	const float along = DotProduct(offset, t.dir);
	const bool passed = along > t.depth + halfLength + 100.0f;
	const bool timeout = now - t.placedAt > 45.0f;
	if (passed || timeout)
	{
		RCBOT_DIAG("[RCBOT2] vehicle obstacle %s userid=%d t=%.2f min_dist=%.0f along=%.0f stopped_near=%.1f "
			"stuck_events=%d maneuver_s=%.1f entry=%.0f speed=%.0f pos=(%.0f %.0f %.0f) tick=%d\n",
			passed ? "passed" : "timeout", userId, now - t.placedAt, t.minDistance, along, t.stoppedNear,
			v.stuckReverses - t.reversesAtStart, t.maneuverSeconds, t.entry, speed, origin.x, origin.y, origin.z,
			gpGlobals->tickcount);
		t.active = false;
	}
}

// A human picks a side once and keeps it. The arc planner re-decided every
// 0.15 s and reached a centred barrier still undecided (obstacle runs 20261119-1405,
// -1418: seen 470-560 units ahead, stopped 1 s later). Pick the side with the
// smaller lateral offset that has a clear sweep beside and past the entity.
bool PlanVehicleDetour(edict_t* driver, edict_t* vehicle, const Vector& origin, const Vector& towards, int entityIndex,
	const int* passable, Vector& point, Vector& exitPoint, Vector& dir, float& offset, int& traces)
{
	edict_t* entity = engine->PEntityOfEntIndex(entityIndex);
	if (entity == nullptr || entity->IsFree() || entity->GetCollideable() == nullptr)
		return false;
	dir = towards - origin;
	dir.z = 0.0f;
	if (dir.NormalizeInPlace() < 1.0f)
		return false;
	const Vector left(-dir.y, dir.x, 0.0f);
	Vector boxMins, boxMaxs;
	entity->GetCollideable()->WorldSpaceSurroundingBounds(&boxMins, &boxMaxs);
	float minA = 1.0e9f, maxA = -1.0e9f, minL = 1.0e9f, maxL = -1.0e9f;
	for (int corner = 0; corner < 4; ++corner)
	{
		const Vector c((corner & 1) ? boxMaxs.x : boxMins.x, (corner & 2) ? boxMaxs.y : boxMins.y, origin.z);
		const Vector d = c - origin;
		const float a = DotProduct(d, dir);
		const float l = DotProduct(d, left);
		minA = (std::min)(minA, a);
		maxA = (std::max)(maxA, a);
		minL = (std::min)(minL, l);
		maxL = (std::max)(maxL, l);
	}
	if (maxA < 0.0f || maxA - minA > 800.0f)
		return false;
	const ICollideable* hull = vehicle->GetCollideable();
	const Vector mins = hull->OBBMins();
	const Vector maxs = hull->OBBMaxs();
	const float halfWidth = (maxs.x - mins.x) * 0.5f;
	const float halfLength = (maxs.y - mins.y) * 0.5f;
	const float clearance = halfWidth + 48.0f;
	float groundZ = origin.z;
	{
		Ray_t ray;
		ray.Init(origin + Vector(0, 0, 80), origin - Vector(0, 0, 180));
		CTraceFilterWorldOnly world;
		trace_t hit;
		enginetrace->TraceRay(ray, MASK_PLAYERSOLID_BRUSHONLY, &world, &hit);
		++traces;
		if (hit.fraction < 1.0f)
			groundZ = hit.endpos.z;
	}
	const float bottom = (std::max)(mins.z, groundZ - origin.z + 30.0f);
	const float top = (std::max)(bottom + 1.0f, maxs.z);
	VehicleManeuverTraceFilter filter(driver, vehicle, origin, passable);
	auto sweepClear = [&](const Vector& a, const Vector& b)
	{
		Ray_t ray;
		ray.Init(a, b, Vector(-halfWidth, -halfWidth, bottom), Vector(halfWidth, halfWidth, top));
		trace_t hit;
		enginetrace->TraceRay(ray, MASK_SOLID, &filter, &hit);
		++traces;
		return !hit.startsolid && hit.fraction >= 1.0f;
	};
	const float options[2] = {maxL + clearance, minL - clearance};
	const int first = std::fabs(options[0]) <= std::fabs(options[1]) ? 0 : 1;
	for (int k = 0; k < 2; ++k)
	{
		const float lateralOffset = options[k == 0 ? first : 1 - first];
		if (std::fabs(lateralOffset) > 350.0f)
			continue;
		const Vector beside = origin + dir * ((minA + maxA) * 0.5f) + left * lateralOffset;
		const Vector behind = origin + dir * (maxA + halfLength + 80.0f) + left * lateralOffset;
		traces += 2;
		if (!GroundAt(beside, origin.z) || !GroundAt(behind, origin.z))
			continue;
		if (!sweepClear(origin, beside) || !sweepClear(beside, behind))
			continue;
		point = beside;
		exitPoint = behind;
		offset = lateralOffset;
		return true;
	}
	return false;
}

// The models face local +Y. Each step sweeps one world-axis box from the
// middle of the body at the old pose to the leading bumper at the next pose.
// A box of half size w / (|cos| + |sin|) spans exactly the body half width w
// across the motion at any heading, so diagonal passages are not narrowed.
// Starting at the body middle keeps a contact at the other end (nose on an
// obstacle while reversing) from making every arc start solid.
//
// Arc 0 follows the route controller: it pursues the lookahead point (its
// line is clear of world geometry by construction) and counts as clear once it
// gets there. A fixed-lock arc ran into the walls of the next road bend
// 1000+ units ahead and caused swerves at full speed (VF runs 20261119-1336..1339).
VehicleManeuver ChooseVehicleManeuver(edict_t* driver, edict_t* vehicle, int chassis, const Vector& origin,
	float heading, float speed, float forwardSpeed, const Vector& goal, const Vector& goalNext, const Vector& routeFrom,
	float routeSteer, float previousSpeed, float previousSteer, bool recover, const int* passable, int& traces)
{
	const ICollideable* hull = vehicle->GetCollideable();
	const Vector mins = hull->OBBMins();
	const Vector maxs = hull->OBBMaxs();
	const float halfWidth = (maxs.x - mins.x) * 0.5f;
	const float halfLength = (maxs.y - mins.y) * 0.5f;
	const float lateral = (mins.x + maxs.x) * 0.5f;
	const float middleY = (mins.y + maxs.y) * 0.5f;
	constexpr float kGroundClearance = 30.0f; // body box starts this far above the ground under the vehicle
	constexpr float decel = 330.0f; // measured straight braking averages 400-450 HU/s²
	constexpr float latency = 0.25f;
	const float step = speed > 450.0f ? 120.0f : 80.0f;
	// Yaw follows a new side input only after ~0.25-0.35 s (turn-test samples).
	const float steerLag = (std::max)(40.0f, speed * 0.3f);
	const float distanceToGoal = Dist2D(origin, goal);
	const float brakeDistance = speed * speed / (2 * decel) + speed * latency;
	// Look further than the stopping distance: an evasive arc needs room to
	// move the body sideways (NF runs saw the barrier only ~230 units ahead).
	const float horizon = (std::min)(1400.0f, (std::max)(500.0f, (std::max)(brakeDistance + step, speed * 1.3f)));
	const float radius = TurnRadius(chassis, speed);
	const float desiredTurn = NormalizeYaw(YawTo(origin, goal) - heading);
	Vector track = goal - routeFrom;
	track.z = 0.0f;
	const float trackLength = track.Length2D();
	if (trackLength > 1.0f)
		track /= trackLength;
	auto offTrack = [&](const Vector& p)
	{
		if (trackLength <= 1.0f)
			return 0.0f;
		Vector d = p - routeFrom;
		d.z = 0.0f;
		const float t = (std::max)(0.0f, (std::min)(trackLength, DotProduct(d, track)));
		return (d - track * t).Length2D();
	};
	VehicleManeuverTraceFilter filter(driver, vehicle, origin, passable);
	CTraceFilterWorldOnly ground;
	auto groundHeight = [&](const Vector& point, float& height)
	{
		Ray_t ray;
		ray.Init(point + Vector(0, 0, 80), point - Vector(0, 0, 180));
		trace_t hit;
		enginetrace->TraceRay(ray, MASK_PLAYERSOLID_BRUSHONLY, &ground, &hit);
		++traces;
		height = hit.endpos.z;
		return hit.fraction < 1.0f && !hit.startsolid;
	};

	VehicleManeuver best;
	float groundZ = 0.0f;
	if ((!recover && forwardSpeed < -80.0f) || !groundHeight(origin, groundZ))
		return best;
	// Box bottom relative to the pose: the hull box reaches below the wheel
	// contact (NF Light mins.z -45 with the origin ~28 above ground).
	const float boxBottom = (std::max)(mins.z, groundZ - origin.z + kGroundClearance);
	const float boxTop = (std::max)(boxBottom + 1.0f, maxs.z);
	float bestScore = 0.0f;
	float routeClear = -1.0f;
	float escapeSteer = routeSteer;
	float escapeClear = -1.0f;
	for (const float direction : {1.0f, -1.0f})
	{
		if ((direction < 0.0f && (forwardSpeed > 80.0f || speed > 220.0f)) ||
			(direction > 0.0f && forwardSpeed < -80.0f))
			continue;
		const float limit = direction > 0.0f ? horizon : 240.0f;
		const float lead = direction > 0.0f ? maxs.y : mins.y;
		const float steering[] = {direction * routeSteer, 0.0f, -0.5f, 0.5f, -1.0f, 1.0f};
		for (std::size_t i = 0; i < sizeof(steering) / sizeof(steering[0]); ++i)
		{
			const bool pursuit = i == 0 && direction > 0.0f;
			const float steer = steering[i];
			if (i > 0 && std::fabs(steer - steering[0]) < 0.1f)
				continue;
			Vector position = origin;
			float yaw = heading;
			float floor = groundZ;
			float clear = 0.0f;
			float nearest = distanceToGoal;
			float lastSteer = previousSteer;
			bool reachedGoal = false;
			bool pastGoal = false; // pursuit: on to the route point after the lookahead point
			for (float distance = step; distance <= limit; distance += step)
			{
				float command = steer;
				if (pursuit)
				{
					// Same law as the route controller: full lock at 30 degrees error.
					const float err = NormalizeYaw(YawTo(position, pastGoal ? goalNext : goal) - yaw);
					command = (std::max)(-1.0f, (std::min)(1.0f, -err / 30.0f));
				}
				// The wheels reach a new lock over steerLag (forward only; a
				// reversing arc starts from a stop with the wheels already set).
				const float blend = direction > 0.0f ?
					(std::min)(1.0f, step / steerLag) : 1.0f;
				const float applied = lastSteer + (command - lastSteer) * blend;
				lastSteer = applied;
				const float turn = -direction * applied * step / radius * (180.0f / kPi);
				const float nextYaw = yaw + turn;
				const Vector axis = YawDir(nextYaw);
				const Vector side = YawDir(nextYaw - 90.0f);
				Vector next = position + YawDir(yaw + turn * 0.5f) * (direction * step);
				float nextFloor = 0.0f;
				if (!groundHeight(next, nextFloor) || std::fabs(nextFloor - floor) > 48.0f ||
					PushAgainst(next, nextYaw + (direction < 0.0f ? 180.0f : 0.0f)))
					break;
				next.z += nextFloor - floor;
				const Vector oldAxis = YawDir(yaw);
				const Vector oldSide = YawDir(yaw - 90.0f);
				const Vector middle = YawDir(yaw + turn * 0.5f);
				const float spread = std::fabs(middle.x) + std::fabs(middle.y); // 1 .. 1.414
				const float padding = halfLength * std::fabs(turn) * kPi / 360.0f + 2.0f;
				const float extent = halfWidth / spread + padding;
				const Vector from = position + oldAxis * middleY + oldSide * lateral;
				const Vector to = next + axis * lead + side * lateral;
				Ray_t ray;
				ray.Init(from, to, Vector(-extent, -extent, boxBottom), Vector(extent, extent, boxTop));
				trace_t hit;
				enginetrace->TraceRay(ray, MASK_SOLID, &filter, &hit);
				++traces;
				// A drivable slope (normal up to ~45 degrees) is ground, not an
				// obstacle: the ground probes above judge steps and drops.
				const bool slope = !hit.startsolid && hit.fraction < 1.0f && hit.plane.normal.z >= 0.7f;
				edict_t* obstacle = hit.m_pEnt != nullptr ? servergameents->BaseEntityToEdict(hit.m_pEnt) : nullptr;
				const int obstacleIndex = obstacle != nullptr ? engine->IndexOfEdict(obstacle) : 0;
				bool kerb = false;
				if (!slope && !hit.startsolid && hit.fraction < 1.0f && obstacleIndex == 0)
				{
					// A world face lower than the ground probes' 48-unit step
					// (kerb, ledge) is climbed, not a wall: sweep again with the
					// box bottom above it. BE VF run 20261119-135959 braked to a
					// stop at such a line the regular driver crosses.
					const float stepBottom = groundZ - origin.z + 50.0f;
					if (stepBottom < boxTop - 1.0f)
					{
						Ray_t high;
						high.Init(from, to, Vector(-extent, -extent, stepBottom), Vector(extent, extent, boxTop));
						trace_t above;
						enginetrace->TraceRay(high, MASK_SOLID, &filter, &above);
						++traces;
						kerb = !above.startsolid && above.fraction >= 1.0f;
					}
				}
				if ((hit.fraction < 1.0f || hit.startsolid) && !slope && !kerb)
				{
					// Overlapping an entity while rolling on means it does not
					// stop vehicles (VF runs: emp_eng_map_brush at 418 HU/s,
					// emp_resource_point at 483 HU/s). Ignore it for a while.
					if (pursuit && hit.startsolid && distance == step && obstacleIndex > 0 && forwardSpeed > 150.0f)
						best.passableCandidate = obstacleIndex;
					if (pursuit)
					{
						best.blocker = obstacleIndex;
						best.blockDistance = distance - step + step * hit.fraction;
						best.blockNormalZ = hit.startsolid ? -2.0f : hit.plane.normal.z;
						best.blockAt = hit.endpos;
						const ICollideable* blockCollide = obstacle != nullptr ? obstacle->GetCollideable() : nullptr;
						best.blockerGroup = blockCollide != nullptr ? blockCollide->GetCollisionGroup() : -1;
						best.blockerSolid = blockCollide != nullptr ? static_cast<int>(blockCollide->GetSolid()) : -1;
						best.blockContents = hit.contents;
						snprintf(best.blockSurface, sizeof(best.blockSurface), "%s",
							hit.surface.name != nullptr ? hit.surface.name : "-");
					}
					break;
				}
				traces += 2;
				if (!GroundAt(next + side * halfWidth, next.z) || !GroundAt(next - side * halfWidth, next.z))
					break;
				clear = distance;
				position = next;
				yaw = nextYaw;
				floor = nextFloor;
				nearest = (std::min)(nearest, Dist2D(position, goal));
				// Follow the route past the lookahead point to its next corner, so an
				// obstacle just behind the lookahead point is seen in time (NF run
				// 20261119-135143 saw the barrier late). Stop at that corner.
				if (pursuit && !pastGoal && Dist2D(position, goal) < step)
					pastGoal = true;
				if (pursuit && pastGoal && Dist2D(position, goalNext) < step)
				{
					reachedGoal = true;
					break;
				}
			}
			const bool horizonClear = reachedGoal || clear + step > limit;
			if (pursuit)
			{
				routeClear = clear;
				// Ground checks and world geometry are handled by regular driving.
				// Take over only for an entity hit on the regular arc or during
				// recovery from a maneuver stall.
				if (!recover && (best.blocker <= 0 || best.passableCandidate > 0))
					return best;
			}
			if (direction > 0.0f && clear > escapeClear + 1.0f)
			{
				escapeClear = clear;
				escapeSteer = pursuit ? routeSteer : steer;
			}
			if (clear < 2 * step)
				continue;
			const float remaining = Dist2D(position, goal);
			const float alignment = std::fabs(NormalizeYaw(
				(remaining > nearest + step ? YawTo(origin, goal) : YawTo(position, goal)) - yaw));
			const float alignmentGain = std::fabs(desiredTurn) - alignment;
			const float progress = distanceToGoal - (direction > 0.0f ? nearest : remaining);
			const float switchCost = previousSpeed * direction < 0.0f ? 45.0f : 0.0f;
			// Prefer arcs that rejoin the route line instead of chasing the
			// lookahead point across open ground, and arcs that stay open: a
			// blocked arc's progress up to the obstacle only leads to a stop
			// (BE run 20261119-1333: kept straight, braked, hit the barrier).
			float score = progress + 2.0f * alignmentGain - switchCost - 16.0f * std::fabs(steer - previousSteer) -
				0.5f * offTrack(position) - (horizonClear ? 0.0f : 1.5f * (limit - clear));
			if (direction < 0.0f)
				score = recover ? 500.0f + clear - switchCost - 16.0f * std::fabs(steer - previousSteer) :
					2.5f * alignmentGain + 0.3f * progress - 65.0f - switchCost;
			const float brakeRoom = clear - step;
			const float reaction = decel * latency;
			const float safeSpeed = horizonClear ? 800.0f :
				std::sqrt(reaction * reaction + 2 * decel * (std::max)(0.0f, brakeRoom)) - reaction;
			if (score > bestScore)
			{
				bestScore = score;
				best.valid = true;
				best.steer = steer;
				// The turn model already ties the arc to the speed; keep only a
				// mild cap for stability at full lock.
				const float turnSpeed = 800.0f - 200.0f * std::fabs(steer);
				best.speed = direction * (std::min)(safeSpeed, direction > 0.0f ? turnSpeed : 180.0f);
			}
		}
	}
	// No usable arc but the route path is blocked: brake to what the free
	// distance allows and steer toward the most open side. Handing back to
	// the route controller here meant full throttle into the obstacle (NF runs
	// 20261119-1330/-1334 sat up on the barrier and gave up).
	if (!best.valid && routeClear >= 0.0f && best.blocker >= 0 && best.passableCandidate < 0)
	{
		const float reaction = decel * latency;
		const float room = (std::max)(0.0f, routeClear - step);
		best.valid = true;
		best.braking = true;
		best.steer = escapeSteer;
		best.speed = (std::max)(0.0f, std::sqrt(reaction * reaction + 2 * decel * room) - reaction);
		// A wheeled vehicle cannot turn standing. With room left before the
		// obstacle, creep on the open lock instead of stopping short (BE VF run
		// 20261119-144123: stopped 143 units before a wall gap and stalled).
		if (best.blockDistance > 100.0f)
			best.speed = (std::max)(best.speed, 120.0f);
	}
	return best;
}

// Vehicle factory bay: the exit ramp is local +X (NF bounds -414..920, BE
// -748..1200, reverse-engineering/notes/vehicle-driving.md section 9).
void FactoryPoints(edict_t* factory, Vector& entrance, Vector& inside)
{
	const Vector origin = CBotGlobals::entityOrigin(factory);
	float yaw = 0.0f;
	if (factory->GetCollideable() != nullptr)
		yaw = factory->GetCollideable()->GetCollisionAngles().y;
	const bool be = factory->GetClassName() != nullptr && std::strstr(factory->GetClassName(), "imp") != nullptr;
	const Vector dir = YawDir(yaw);
	entrance = origin + dir * (be ? 1450.0f : 1170.0f);
	inside = origin + dir * (be ? 350.0f : 300.0f);
}

// Vehicle factory spawn pad (attachment "build", vehicle-driving.md section 9).
Vector FactoryBuildSpot(edict_t* factory)
{
	const Vector origin = CBotGlobals::entityOrigin(factory);
	float yaw = 0.0f;
	if (factory->GetCollideable() != nullptr)
		yaw = factory->GetCollideable()->GetCollisionAngles().y;
	const bool be = factory->GetClassName() != nullptr && std::strstr(factory->GetClassName(), "imp") != nullptr;
	if (be)
		return origin + Vector(0.0f, 0.0f, 200.0f);
	return origin + YawDir(yaw) * 172.5f + Vector(0.0f, 0.0f, 172.5f);
}

bool IsNearFactoryBay(edict_t* factory, const Vector& pos, const float radius = 350.0f)
{
	if (factory == nullptr || factory->IsFree())
		return false;
	Vector entrance, inside;
	FactoryPoints(factory, entrance, inside);
	const Vector a = FactoryBuildSpot(factory);
	const Vector ab(entrance.x - a.x, entrance.y - a.y, 0.0f);
	const float lenSq = ab.x * ab.x + ab.y * ab.y;
	float t = 0.0f;
	if (lenSq > 1.0f)
		t = (std::max)(0.0f, (std::min)(1.0f, ((pos.x - a.x) * ab.x + (pos.y - a.y) * ab.y) / lenSq));
	const Vector closest(a.x + ab.x * t, a.y + ab.y * t, a.z);
	return Dist2D(pos, closest) < radius;
}

bool HasNearbyHumanPlayer(const int team, const Vector& pos, const float radius)
{
	for (int p = 1; p <= gpGlobals->maxClients; ++p)
	{
		edict_t* player = engine->PEntityOfEntIndex(p);
		if (player == nullptr || player->IsFree() || !CBotGlobals::entityIsAlive(player))
			continue;
		if (CClassInterface::getTeam(player) != team)
			continue;
		if (CBots::getBotPointer(player) != nullptr || Dist2D(CBotGlobals::entityOrigin(player), pos) >= radius)
			continue;
		IPlayerInfo* info = playerinfomanager != nullptr ? playerinfomanager->GetPlayerInfo(player) : nullptr;
		if (info == nullptr || !info->IsFakeClient())
			return true;
	}
	return false;
}

// Per-vehicle empty duration and bot-abandon history (3.26 3b / 3.17).
struct VehicleEmptyTrack
{
	float emptySince = -1.0f;
	Vector emptyAt = Vector(0.0f, 0.0f, 0.0f);
	int serial = -1;
};

struct BotAbandonRecord
{
	int serial = -1;
	int lastDriverUserId = -1;
	float abandonedAt = 0.0f;
	Vector abandonedPos = Vector(0.0f, 0.0f, 0.0f);
	int abandonCount = 0;
};

VehicleEmptyTrack g_vehicleEmpty[MAX_EDICTS];
BotAbandonRecord g_botAbandon[MAX_EDICTS];

float UpdateVehicleEmptyTime(edict_t* e, const int idx, const int driver, const float now)
{
	if (e == nullptr || idx < 0 || idx >= MAX_EDICTS)
		return 0.0f;
	VehicleEmptyTrack& tr = g_vehicleEmpty[idx];
	const int serial = e->m_NetworkSerialNumber;
	const Vector at = CBotGlobals::entityOrigin(e);
	if (driver >= 0)
	{
		tr.emptySince = -1.0f;
		return 0.0f;
	}
	if (tr.serial != serial || (tr.emptyAt - at).Length() > 100.0f ||
		tr.emptySince > now || tr.emptySince < 0.0f)
	{
		tr.serial = serial;
		tr.emptySince = now;
		tr.emptyAt = at;
	}
	return now - tr.emptySince;
}

void RecordBotVehicleAbandon(edict_t* e, const int idx, const int userId, const Vector& pos, const float now)
{
	if (e == nullptr || idx < 0 || idx >= MAX_EDICTS)
		return;
	BotAbandonRecord& rec = g_botAbandon[idx];
	const int serial = e->m_NetworkSerialNumber;
	if (rec.serial == serial && Dist2D(pos, rec.abandonedPos) < 400.0f)
		++rec.abandonCount;
	else
		rec.abandonCount = 1;
	rec.serial = serial;
	rec.lastDriverUserId = userId;
	rec.abandonedAt = now;
	rec.abandonedPos = pos;
}

void ClearBotVehicleAbandonIfMoved(edict_t* e, const int idx, const Vector& pos)
{
	if (e == nullptr || idx < 0 || idx >= MAX_EDICTS)
		return;
	BotAbandonRecord& rec = g_botAbandon[idx];
	if (rec.serial == e->m_NetworkSerialNumber && rec.abandonCount > 0 &&
		Dist2D(pos, rec.abandonedPos) > 400.0f)
	{
		rec.abandonCount = 0;
		rec.abandonedAt = 0.0f;
	}
}

// Way around the factory to its ramp: the mesh still has areas under a
// factory built after generation, so a plain route ran into its back wall
// (run 205248). Corners outside the hull, same side as the bot.
std::vector<Vector> FactoryRoute(edict_t* factory, const Vector& from)
{
	const Vector origin = CBotGlobals::entityOrigin(factory);
	float yaw = 0.0f;
	if (factory->GetCollideable() != nullptr)
		yaw = factory->GetCollideable()->GetCollisionAngles().y;
	const bool be = factory->GetClassName() != nullptr && std::strstr(factory->GetClassName(), "imp") != nullptr;
	const Vector fwd = YawDir(yaw);
	const Vector left(-fwd.y, fwd.x, 0.0f);
	const float localX = DotProduct(from - origin, fwd);
	const float localY = DotProduct(from - origin, left);
	const float side = localY >= 0.0f ? 1.0f : -1.0f;
	const float sideOut = be ? 900.0f : 1050.0f;
	const float backOut = be ? -950.0f : -600.0f;
	const float frontOut = be ? 1400.0f : 1150.0f;
	std::vector<Vector> route;
	if (localX < frontOut - 100.0f || std::fabs(localY) < sideOut - 100.0f)
	{
		if (localX < 0.0f)
			route.push_back(origin + fwd * backOut + left * (side * sideOut));
		route.push_back(origin + fwd * frontOut + left * (side * sideOut));
	}
	Vector entrance, inside;
	FactoryPoints(factory, entrance, inside);
	route.push_back(entrance);
	// The points lie around the factory, so they take its height, not the
	// buyer's (Palmbay session 20261120-205119: buyers ~900 below the plateau
	// factory got goals at their own height; the navmesh put the goal in the
	// buyer's own area, "goal reached" at once, no movement for 90-200 s).
	for (Vector& point : route)
	{
		point.z = origin.z;
		Vector mesh;
		if (RCBotNavMesh::IsLoaded() && RCBotNavMesh::ClosestPointOnMesh(point, 200.0f, mesh))
			point = mesh;
	}
	return route;
}

// The game's CTraceFilterNoNPCsOrPlayer is a CTraceFilterSimple with
// COLLISION_GROUP_NONE: CGameRules::ShouldCollide (0x3fd760) rejects groups
// 10 (in vehicle), 12 (vehicle clip) and 15 against NONE. The bay's
// emp_building_vehicle_physbox is group 12 (CEmpBuildingVehiclePhysbox::Spawn
// 0x61971b), so the game's trace passes through it (run 113715: bought in the
// bay although the plain trace hit the physbox).
class CTraceFilterGameNone : public CTraceFilterNoPlayers
{
public:
	bool ShouldHitEntity(IHandleEntity* handle, int mask) override
	{
		if (!CTraceFilterNoPlayers::ShouldHitEntity(handle, mask))
			return false;
		IServerUnknown* unknown = static_cast<IServerUnknown*>(handle);
		ICollideable* collide = unknown != nullptr ? unknown->GetCollideable() : nullptr;
		if (collide == nullptr)
			return true;
		const int group = collide->GetCollisionGroup();
		return group != 10 && group != 12 && group != 15;
	}
};

// Repair pads are drive-through with two open ends (user). Trace a
// vehicle-wide hull from outside each side of the station's box toward its
// centre; sides that get through are entrances. Returns the open entry
// points (300 outside the box) and logs every side.
std::vector<Vector> RepairPadEntrances(edict_t* station, float z, char* why, size_t whySize)
{
	std::vector<Vector> open;
	ICollideable* c = station->GetCollideable();
	if (c == nullptr || enginetrace == nullptr)
		return open;
	const Vector centre = CBotGlobals::entityOrigin(station);
	const float yaw = c->GetCollisionAngles().y;
	const Vector mins = c->OBBMins(), maxs = c->OBBMaxs();
	const float extent[4] = {maxs.x, maxs.y, -mins.x, -mins.y};
	size_t used = 0;
	if (why != nullptr && whySize > 0)
		why[0] = '\0';
	for (int k = 0; k < 4; ++k)
	{
		const Vector dir = YawDir(yaw + 90.0f * k);
		Vector start = centre + dir * (extent[k] + 200.0f);
		Vector end = centre + dir * 100.0f;
		start.z = end.z = z + 40.0f;
		Ray_t ray;
		ray.Init(start, end, Vector(-60, -60, 0), Vector(60, 60, 60));
		CTraceFilterGameNone filter;
		trace_t hit;
		enginetrace->TraceRay(ray, MASK_SOLID, &filter, &hit);
		const float reached = Dist2D(hit.endpos, centre);
		const bool isOpen = !hit.startsolid && reached < extent[k] * 0.5f;
		if (isOpen)
		{
			Vector entry = centre + dir * (extent[k] + 300.0f);
			entry.z = z;
			open.push_back(entry);
		}
		if (why != nullptr && used < whySize)
		{
			const char* cls = "none";
			if (hit.m_pEnt != nullptr && hit.fraction < 1.0f)
			{
				IServerNetworkable* n = reinterpret_cast<IServerUnknown*>(hit.m_pEnt)->GetNetworkable();
				edict_t* he = n != nullptr ? n->GetEdict() : nullptr;
				cls = he != nullptr && he->GetClassName() != nullptr ? he->GetClassName() : "?";
			}
			const int n = snprintf(why + used, whySize - used, "side%d yaw=%.0f ext=%.0f reach=%.0f ss=%d hit=%s %s; ",
				k, NormalizeYaw(yaw + 90.0f * k), extent[k], reached, hit.startsolid ? 1 : 0, cls,
				isOpen ? "OPEN" : "closed");
			if (n > 0)
				used += static_cast<size_t>(n);
		}
	}
	return open;
}

// CEmpPlayer::IsInFactory (0x65fe80): player hull traced 1024 up (GameFactoryRoofHit) and 1024 down with
// CTraceFilterNoNPCsOrPlayer and MASK_SOLID must hit the factory first, and
// the factory must be the player's handle +0x1454 (set by Touch or by
// emp_vehicle_factory <index>, Command_emp_vehicle_factory 0x64d090).
edict_t* GameFactoryTraceHit(const Vector& at, bool* startSolid = nullptr, float* fraction = nullptr)
{
	if (enginetrace == nullptr)
		return nullptr;
	Ray_t ray;
	ray.Init(at, at - Vector(0, 0, 1024), Vector(-16, -16, 0), Vector(16, 16, 72));
	CTraceFilterGameNone filter;
	trace_t hit;
	enginetrace->TraceRay(ray, MASK_SOLID, &filter, &hit);
	if (startSolid != nullptr)
		*startSolid = hit.startsolid;
	if (fraction != nullptr)
		*fraction = hit.fraction;
	if (hit.m_pEnt == nullptr)
		return nullptr;
	IServerNetworkable* networkable = reinterpret_cast<IServerUnknown*>(hit.m_pEnt)->GetNetworkable();
	return networkable != nullptr ? networkable->GetEdict() : nullptr;
}

// IsInFactory traces the hull UP first (origin + 1024, TraceRay 0x6601d2, gdb 2026-11-14):
// the buyer must stand under the factory roof; on the ramp or apron the purchase fails silently
// (run 103144: 20 purchases, no vehicle).
edict_t* GameFactoryRoofHit(const Vector& at)
{
	if (enginetrace == nullptr)
		return nullptr;
	Ray_t ray;
	ray.Init(at, at + Vector(0, 0, 1024), Vector(-16, -16, 0), Vector(16, 16, 72));
	CTraceFilterGameNone filter;
	trace_t hit;
	enginetrace->TraceRay(ray, MASK_SOLID, &filter, &hit);
	if (hit.m_pEnt == nullptr)
		return nullptr;
	IServerNetworkable* networkable = reinterpret_cast<IServerUnknown*>(hit.m_pEnt)->GetNetworkable();
	return networkable != nullptr ? networkable->GetEdict() : nullptr;
}

bool StandingInFactory(edict_t* player, edict_t* factory)
{
	if (player == nullptr || factory == nullptr)
		return false;
	const Vector at = CBotGlobals::entityOrigin(player);
	return GameFactoryTraceHit(at) == factory && GameFactoryRoofHit(at) == factory;
}

// Fallback when the bot is not in the factory at the bay centre: spots where
// IsInFactory holds. Each column's factory surfaces (line ray against the
// factory only, restarted below each hit) are tested with the game condition:
// player hull free, and the hull traced down from there hits the factory.
bool FindFactoryStandSpot(edict_t* factory, const Vector& near, Vector& spot, int& candidates,
	char* why = nullptr, size_t whySize = 0)
{
	candidates = 0;
	int surfaces = 0, blocked = 0, notFactory = 0;
	if (factory->GetCollideable() == nullptr || enginetrace == nullptr)
		return false;
	Vector mins, maxs;
	factory->GetCollideable()->WorldSpaceSurroundingBounds(&mins, &maxs);
	const float floorTop = CBotGlobals::entityOrigin(factory).z + 200.0f; // skip roofs
	float best = 1.0e9f;
	for (float x = mins.x + 16.0f; x <= maxs.x - 16.0f; x += 32.0f)
		for (float y = mins.y + 16.0f; y <= maxs.y - 16.0f; y += 32.0f)
		{
			Vector start(x, y, maxs.z + 8.0f);
			const Vector end(x, y, mins.z - 64.0f);
			for (int it = 0; it < 10 && start.z > end.z; ++it)
			{
				Ray_t ray;
				ray.Init(start, end);
				trace_t hit;
				enginetrace->ClipRayToEntity(ray, MASK_SOLID, factory->GetUnknown(), &hit);
				if (hit.startsolid)
				{
					start.z = (std::min)(start.z + (end.z - start.z) * hit.fractionleftsolid, start.z) - 2.0f;
					continue;
				}
				if (hit.fraction >= 1.0f)
					break;
				const Vector surface = hit.endpos;
				start.z = surface.z - 2.0f;
				if (surface.z > floorTop)
					continue;
				++surfaces;
				const Vector p = surface + Vector(0, 0, 2);
				Ray_t box;
				box.Init(p, p, Vector(-16, -16, 0), Vector(16, 16, 72));
				CTraceFilterGameNone filter;
				trace_t occupied;
				enginetrace->TraceRay(box, MASK_PLAYERSOLID, &filter, &occupied);
				if (occupied.startsolid || occupied.allsolid)
				{
					++blocked;
					continue;
				}
				if (GameFactoryTraceHit(p) != factory || GameFactoryRoofHit(p) != factory)
				{
					++notFactory;
					continue;
				}
				++candidates;
				const float d = (p - near).Length() + 2.0f * std::fabs(p.z - near.z);
				if (d < best)
				{
					best = d;
					spot = p;
				}
			}
		}
	if (why != nullptr && whySize > 0)
		snprintf(why, whySize, "surfaces=%d blocked=%d not_factory=%d bounds=(%.0f %.0f %.0f)-(%.0f %.0f %.0f)",
			surfaces, blocked, notFactory, mins.x, mins.y, mins.z, maxs.x, maxs.y, maxs.z);
	return candidates > 0;
}

// Waiting spots claimed by the vehicles of a team (front support, APC park, staging). A claim lasts
// 3 s and is renewed by every goal pick (0.5 s) while the vehicle keeps that goal, so it lapses
// when the vehicle dies or takes another goal.
struct WaitSpotClaim
{
	int team = 0;
	int vehicle = 0;
	Vector spot = Vector(0, 0, 0);
	float until = 0.0f;
};
constexpr float kWaitSpotSpacing = 500.0f;
constexpr float kWaitSpotHold = 3.0f;
WaitSpotClaim g_waitSpotClaims[64];

void ClaimWaitSpot(int team, int vehicle, const Vector& spot, float now)
{
	if (!rcbot_empires_vehicle_spread_wait.GetBool() || vehicle <= 0)
		return;
	WaitSpotClaim* slot = nullptr;
	for (WaitSpotClaim& claim : g_waitSpotClaims)
		if (claim.vehicle == vehicle)
		{
			slot = &claim;
			break;
		}
	if (slot == nullptr)
		for (WaitSpotClaim& claim : g_waitSpotClaims)
			if (claim.until <= now || claim.until > now + 60.0f) // expired, or left from a previous map
			{
				slot = &claim;
				break;
			}
	if (slot == nullptr)
		return;
	slot->team = team;
	slot->vehicle = vehicle;
	slot->spot = spot;
	slot->until = now + kWaitSpotHold;
}

bool WaitSpotTaken(int team, int vehicle, const Vector& spot, float now)
{
	if (!rcbot_empires_vehicle_spread_wait.GetBool())
		return false;
	for (const WaitSpotClaim& claim : g_waitSpotClaims)
		if (claim.team == team && claim.vehicle != vehicle && claim.until > now && claim.until <= now + 60.0f &&
			Dist2D(claim.spot, spot) < kWaitSpotSpacing)
			return true;
	return false;
}

// Open flat ground for a vehicle: road under centre and corners, no solid in
// a vehicle-sized box above it.
bool VehicleSpotFree(const Vector& spot)
{
	static const float kCorners[][2] = {{0, 0}, {220, 220}, {220, -220}, {-220, 220}, {-220, -220}};
	for (const auto& c : kCorners)
		if (!GroundAt(spot + Vector(c[0], c[1], 0), spot.z))
			return false;
	if (enginetrace == nullptr)
		return true;
	Ray_t ray;
	ray.Init(spot + Vector(0, 0, 60), spot + Vector(0, 0, 61), Vector(-200, -200, 0), Vector(200, 200, 260));
	CTraceFilterNoPlayers filter;
	trace_t hit;
	enginetrace->TraceRay(ray, MASK_PLAYERSOLID, &filter, &hit);
	return !hit.startsolid && !hit.allsolid && hit.fraction >= 1.0f;
}

bool EnemyCommanderOrigin(int enemyTeam, Vector& out)
{
	const char* cls = enemyTeam == 2 ? "emp_nf_commander" : "emp_imp_commander";
	for (int i = gpGlobals->maxClients + 1; i < gpGlobals->maxEntities; ++i)
	{
		edict_t* e = engine->PEntityOfEntIndex(i);
		if (IsClass(e, cls))
		{
			out = CBotGlobals::entityOrigin(e);
			return true;
		}
	}
	return false;
}

bool DebugBuildVehicles()
{
	ConVar* var = g_pCVar != nullptr ? g_pCVar->FindVar("emp_sv_debug_build_vehicles") : nullptr;
	return var != nullptr && var->GetInt() != 0;
}

Vector EntityCenter(edict_t* e)
{
	Vector origin = CBotGlobals::entityOrigin(e);
	if (e != nullptr && e->GetCollideable() != nullptr)
	{
		Vector mins, maxs;
		e->GetCollideable()->WorldSpaceSurroundingBounds(&mins, &maxs);
		if (maxs.z > mins.z)
		{
			Vector centre = (mins + maxs) * 0.5f;
			// Infantry: aim at solid upper chest (~49 HU standing, ~22 HU crouching) instead of
			// 50% (36 HU = leg gap), where the cannon convergence ray passes between a stationary
			// soldier's legs onto the ground behind him and shifts offset MGs off target.
			const int entIndex = engine != nullptr ? engine->IndexOfEdict(e) : -1;
			if (gpGlobals != nullptr && entIndex >= 1 && entIndex <= gpGlobals->maxClients)
			{
				centre.z = mins.z + EmpiresVehicleAim::InfantryAimHeight(maxs.z - mins.z);
				return centre;
			}
			// Building bounds reach far below the ground (map refinery 132: z -326..32, ground -190):
			// the box centre sat 40 HU above ground and shells hit the pad in front (run 125847:
			// 11 cannon shots, refinery stayed 150/150). Aim halfway between origin and roof.
			const char* cls = e->GetClassName();
			if (cls != nullptr && std::strncmp(cls, "emp_building_", 13) == 0 && maxs.z > origin.z)
				centre.z = (std::max)(centre.z, origin.z + (maxs.z - origin.z) * 0.5f);
			return centre;
		}
	}
	return origin + Vector(0, 0, 40);
}
} // namespace

// Run-over decision for an armed vehicle against a soldier on foot (TODO from human test 26.11.).
// Starts when the MG fired 2 s at him without damage, or he stands below the gun's reach (pitch
// limit), and the straight way is short, level and has ground; lasts up to 6 s, then the soldier is
// left alone for 10 s (stall-escalate stage 3) so the tank does not chase him across the map.
bool CBotEmpires::wantEmpiresRunOver(EmpiresVehicleBrain& v, edict_t* target, const Vector& targetAt,
	const Vector& origin, float now, int userId)
{
	const int idx = engine->IndexOfEdict(target);
	if (!rcbot_empires_vehicle_runover.GetBool() || v.commanderCV || idx < 1 || idx > gpGlobals->maxClients)
	{
		v.runOverTarget = -1;
		return false;
	}
	const float distance = Dist2D(origin, targetAt);
	const bool reachable = distance < 1500.0f && std::fabs(targetAt.z - origin.z) < 120.0f &&
		SafeHeading(origin, YawTo(origin, targetAt), (std::min)(distance + 200.0f, 1200.0f));
	if (v.runOverTarget == idx)
	{
		if (now < v.runOverUntil && reachable)
			return true;
		RCBOT_DIAG("[RCBOT2] stall-escalate who=vehicle-mg userid=%d stage=3 reason=run-over-%s target=%d dist=%.0f\n",
			userId, reachable ? "timeout" : "unreachable", idx, distance);
		v.fireBlockedSkipTarget = idx;
		v.fireBlockedSkipUntil = now + 10.0f;
		v.target = nullptr;
		v.nextTargetPick = now;
		v.runOverTarget = -1;
		return false;
	}
	const bool noHit = v.mgEngTarget == idx && v.mgEngHp0 > 0 && v.mgEngHpLast >= v.mgEngHp0 && v.mgEngFired >= 2.0f;
	const bool belowGun = now < v.pitchLimitUntil && v.pitchLimitDown;
	if (!(noHit || belowGun) || !reachable)
		return false;
	v.runOverTarget = idx;
	v.runOverUntil = now + 6.0f;
	RCBOT_DIAG("[RCBOT2] stall-escalate who=vehicle-mg userid=%d stage=2 reason=run-over target=%d dist=%.0f "
		"why=%s fired=%.1f\n", userId, idx, distance, noHit ? "no-damage" : "below-gun", v.mgEngFired);
	return true;
}


bool CBotEmpires::isEmpiresBillNye() const
{
	int team = rcbot_empires_billnye.GetInt();
	if (team == -1)
	{
		const char* map = gpGlobals != nullptr ? STRING(gpGlobals->mapname) : nullptr;
		if (map == nullptr || std::strstr(map, "rainbowroad") == nullptr)
			return false;
		team = 2;
	}
	if (team != 2 && team != 3)
		return false;
	if (CClassInterface::getTeam(m_pEdict) != team)
		return false;

	// The first living Scout bot of that team is Bill Nye.
	for (int slot = 0; slot < RCBOT_MAXPLAYERS; ++slot)
	{
		CBot* bot = CBots::getBot(slot);
		if (bot == nullptr || !bot->inUse() || bot->getEdict() == nullptr ||
			CClassInterface::getTeam(bot->getEdict()) != team)
			continue;
		CBotEmpires* candidate = static_cast<CBotEmpires*>(bot);
		int classId = -1;
		bool desired = false, desiredKnown = false;
		const char* prop = nullptr;
		if (!candidate->readEmpiresClass(candidate->getEdict(), classId, desired,
			desiredKnown, prop) || classId != 0)
			continue;
		return candidate == this;
	}
	return false;
}

void CBotEmpires::resetEmpiresVehicle(const char* reason, float cooldown)
{
	if (m_vehicle.stage != EmpiresVehicleStage::NONE)
		RCBOT_DIAG("[RCBOT2] vehicle reset userid=%d stage=%s reason=%s cooldown=%.0f\n",
			engine->GetPlayerUserId(m_pEdict), StageName(m_vehicle.stage), reason, cooldown);
	if (g_obstacleTest.active && g_obstacleTest.userId == engine->GetPlayerUserId(m_pEdict))
	{
		RCBOT_DIAG("[RCBOT2] vehicle obstacle abort userid=%d reason=%s t=%.2f\n", g_obstacleTest.userId, reason,
			gpGlobals->curtime - g_obstacleTest.placedAt);
		g_obstacleTest.active = false;
	}
	const float now = gpGlobals->curtime;
	const int failures = m_vehicle.failures;
	const bool wantJeep = m_vehicle.wantJeep;
	const int abandonedVehicle = m_vehicle.abandonedVehicle;
	const float abandonedUntil = m_vehicle.abandonedUntil;
	m_vehicle = EmpiresVehicleBrain();
	m_vehicle.failures = failures;
	m_vehicle.wantJeep = wantJeep;
	m_vehicle.abandonedVehicle = abandonedVehicle;
	m_vehicle.abandonedUntil = abandonedUntil;
	m_vehicle.cooldownUntil = now + cooldown;
	m_vehicle.nextDecision = now + 1.0f;
}

bool CBotEmpires::applyEmpiresVehicleCommand(CBotCmd& command)
{
	switch (m_vehicle.stage)
	{
	case EmpiresVehicleStage::BUYING:
	case EmpiresVehicleStage::DRIVING:
	case EmpiresVehicleStage::STUN_EXIT:
	case EmpiresVehicleStage::GIVE_UP_EXIT:
	case EmpiresVehicleStage::ENTERING:
	case EmpiresVehicleStage::STUN_THROW:
		break;
	case EmpiresVehicleStage::REPAIR_ASSIST:
		// Walking uses the normal movement (setMoveTo); a raw forwardmove did
		// not move the engineer (run 224451). Only aiming/repairing overrides.
		if (!m_vehicle.assistOverride)
			return false;
		break;
	default:
		return false;
	}
	command.forwardmove = m_vehicle.forward;
	command.sidemove = m_vehicle.side;
	command.upmove = 0.0f;
	command.buttons = m_vehicle.buttons;
	command.viewangles = m_vehicle.view;
	if (m_vehicle.stage == EmpiresVehicleStage::STUN_THROW ||
		m_vehicle.stage == EmpiresVehicleStage::REPAIR_ASSIST)
		command.weaponselect = m_iSelectWeapon;
	return true;
}

// A living bot on foot with no world floor within 2000 below and no navmesh area
// within 1500 for 2 s has nothing to plan from (fell off rainbowroad): respawn.
void CBotEmpires::tickEmpiresVoidSuicide()
{
	if (gpGlobals == nullptr || engine == nullptr || enginetrace == nullptr || helpers == nullptr || m_pEdict == nullptr ||
		m_pEdict->IsFree())
		return;
	const float now = gpGlobals->curtime;
	if (!isAlive() || m_pPlayerInfo == nullptr || m_pPlayerInfo->IsDead() ||
		!RCBotNavMesh::IsLoaded() || m_vehicle.stage != EmpiresVehicleStage::NONE ||
		commanderVehicleEntryActive() || m_iCommanderEntryStage != 0 ||
		EmpiresVehiclePlayerVehicle(this, m_pEdict) >= 0)
	{
		m_flVoidSince = -1.0f;
		m_bVoidKilled = false;
		return;
	}
	if (now < m_flVoidNextCheck)
		return;
	m_flVoidNextCheck = now + 1.0f;
	const Vector pos = getOrigin();
	Ray_t ray;
	ray.Init(pos, pos - Vector(0, 0, 2000.0f));
	CTraceFilterWorldOnly filter;
	trace_t hit;
	enginetrace->TraceRay(ray, MASK_PLAYERSOLID_BRUSHONLY, &filter, &hit);
	Vector mesh;
	const bool floor = hit.fraction < 1.0f || hit.startsolid || hit.allsolid;
	if (floor || RCBotNavMesh::ClosestPointOnMesh(pos, 1500.0f, mesh))
	{
		m_flVoidSince = -1.0f;
		m_bVoidKilled = false;
		return;
	}
	if (m_flVoidSince < 0.0f)
		m_flVoidSince = now;
	if (now - m_flVoidSince < 2.0f || m_bVoidKilled)
		return;
	m_bVoidKilled = true;
	RCBOT_DIAG("[RCBOT2] void-suicide userid=%d pos=(%.0f %.0f %.0f)\n", engine->GetPlayerUserId(m_pEdict),
		pos.x, pos.y, pos.z);
	helpers->ClientCommand(m_pEdict, "kill\n");
}

bool EmpiresEngibotWithinLeash(const edict_t* player, const Vector& at); // bot_empires_engibot.cpp

namespace
{
bool VehicleNeedsBaseRepair(const CBotEmpires* bot, edict_t* e, float maxSpeed, int& worstSide, bool& armorDamaged)
{
	worstSide = -1;
	armorDamaged = false;
	if (bot == nullptr || e == nullptr || e->IsFree() || !IsClass(e, "emp_vehicle") || !EmpiresVehicleAlive(bot, e))
		return false;
	if (EmpiresVehicleExceedsSpeed(e, maxSpeed))
		return false;
	float hp = -1.0f, maxHp = -1.0f;
	const bool hullDamaged = ReadVehicleHealth(e, hp) && ReadVehicleMaxHealth(e, maxHp) &&
		hp > 0.0f && maxHp > 0.0f && hp < maxHp - 0.5f;
	const int armorMax = EmpiresVehicleReadInt(bot, e, "m_iMaxArmorHealth", 0);
	int worstVal = armorMax;
	if (armorMax > 0)
	{
		for (int k = 0; k < 4; ++k)
		{
			const int a = ReadArmorSide(e, k);
			// Ignore 0: fresh vehicles build armor up from 0 and unarmored sides stay 0.
			if (a > 0 && a < armorMax)
			{
				armorDamaged = true;
				if (worstSide < 0 || a < worstVal)
				{
					worstVal = a;
					worstSide = k;
				}
			}
		}
	}
	return hullDamaged || armorDamaged;
}

edict_t* FindBaseVehicleForEngibotRepair(CBotEmpires* bot, int team, int& worstSide, bool& armorDamaged)
{
	worstSide = -1;
	armorDamaged = false;
	if (bot == nullptr || engine == nullptr || gpGlobals == nullptr)
		return nullptr;
	std::vector<Vector> pads;
	for (int i = gpGlobals->maxClients + 1; i < gpGlobals->maxEntities; ++i)
	{
		edict_t* e = engine->PEntityOfEntIndex(i);
		if (e == nullptr || e->IsFree() || e->GetClassName() == nullptr)
			continue;
		const char* cls = e->GetClassName();
		if (std::strncmp(cls, "emp_building_", 13) == 0 && std::strstr(cls, "repair") != nullptr &&
			CClassInterface::getTeam(e) == team)
			pads.push_back(CBotGlobals::entityOrigin(e));
	}
	const Vector origin = bot->getOrigin();
	edict_t* best = nullptr;
	bool bestOnPad = false;
	float bestDist = 1.0e9f;
	for (int i = gpGlobals->maxClients + 1; i < gpGlobals->maxEntities; ++i)
	{
		edict_t* e = engine->PEntityOfEntIndex(i);
		if (e == nullptr || e->IsFree() || !IsClass(e, "emp_vehicle") ||
			EmpiresVehicleReadInt(bot, e, "m_iTeamNum", -1) != team)
			continue;
		const Vector at = CBotGlobals::entityOrigin(e);
		if (!EmpiresEngibotWithinLeash(bot->getEdict(), at) || !bot->empiresRepairCallAllowed(e))
			continue;
		int side = -1;
		bool armor = false;
		if (!VehicleNeedsBaseRepair(bot, e, 80.0f, side, armor))
			continue;
		bool onPad = false;
		for (const Vector& pad : pads)
			if (Dist2D(at, pad) < 450.0f)
			{
				onPad = true;
				break;
			}
		const float d = Dist2D(origin, at);
		if (best == nullptr || (onPad && !bestOnPad) || (onPad == bestOnPad && d < bestDist))
		{
			best = e;
			bestOnPad = onPad;
			bestDist = d;
			worstSide = side;
			armorDamaged = armor;
		}
	}
	return best;
}
} // namespace

void CBotEmpires::tickEmpiresVehicle()
{
	if (gpGlobals == nullptr || engine == nullptr || m_pEdict == nullptr || m_pEdict->IsFree())
		return;
	const float now = gpGlobals->curtime;
	EmpiresVehicleBrain& v = m_vehicle;
	v.forward = v.side = 0.0f;
	v.buttons = 0;

	if (!isAlive() || m_pPlayerInfo == nullptr || m_pPlayerInfo->IsDead())
	{
		if (v.stage != EmpiresVehicleStage::NONE)
			resetEmpiresVehicle("dead", 3.0f);
		return;
	}
	// An engineer with a build order who spawned in an APC seat must still get out (run
	// 20261121-135318: BE engineer userid=9 sat in APC 572 until its order stalled at 232/250).
	// The own bot commander's team-director order (push/hold/defense, TODO 5.8) does not block buying or
	// driving: the order ends here locally and the commander drops it once the bot has no role any more
	// (crossroads dev_check pad 20261121-153156: the buy waited 200 s for a push to a resource point).
	const bool directorOrder = isEmpiresCommanderMoveActive() &&
		EmpiresCommanderOrderIsOurs(getTeam(), engine->IndexOfEdict(m_pEdict));
	if (directorOrder && v.stage != EmpiresVehicleStage::NONE)
	{
		RCBOT_DIAG("[RCBOT2] vehicle director-order-dropped userid=%d stage=%s\n", engine->GetPlayerUserId(m_pEdict),
			StageName(v.stage));
		m_bEmpiresCommanderMoveFailed = true;
		m_bEmpiresCommanderMoveStuck = false;
		// The dropped order's route and schedule must not keep steering the buyer/driver away.
		cancelNavigationAttempt("vehicle-director-drop");
		if (m_pSchedules != nullptr)
			m_pSchedules->freeMemory();
		if (m_pNavigator != nullptr)
			m_pNavigator->clear();
	}
	const bool commanderMoveBlocks = isEmpiresCommanderMoveActive() && !directorOrder;
	const bool buildAssignPassengerExit = m_iBuildAssignStage > 0 && !commanderVehicleEntryActive() &&
		!commanderMoveBlocks && !isEmpiresVehicleActionActive() &&
		((v.stage == EmpiresVehicleStage::NONE && EmpiresVehiclePlayerVehicle(this, m_pEdict) >= 0) ||
		 (v.stage == EmpiresVehicleStage::GIVE_UP_EXIT && v.passengerExit));
	if (!buildAssignPassengerExit && (commanderVehicleEntryActive() || m_iBuildAssignStage > 0 ||
		commanderMoveBlocks || isEmpiresVehicleActionActive()))
	{
		if (v.stage != EmpiresVehicleStage::NONE &&
			v.stage != EmpiresVehicleStage::DRIVING)
			resetEmpiresVehicle("other-owner", 5.0f);
		if (v.stage == EmpiresVehicleStage::NONE)
			return;
	}

	const int userId = engine->GetPlayerUserId(m_pEdict);
	const int client = engine->IndexOfEdict(m_pEdict);
	const int team = getTeam();
	const int enemyTeam = team == 2 ? 3 : 2;
	const int inVehicle = EmpiresVehiclePlayerVehicle(this, m_pEdict);

	auto setStage = [&](EmpiresVehicleStage stage, float timeout) {
		RCBOT_DIAG("[RCBOT2] vehicle stage userid=%d %s->%s bill_nye=%d vehicle=%d\n", userId,
			StageName(v.stage), StageName(stage), v.billNye ? 1 : 0, v.vehicleIndex);
		v.stage = stage;
		v.stageSince = now;
		v.stageUntil = timeout > 0.0f ? now + timeout : 0.0f;
	};
	// Walking to the factory/buy spot: drop the running goal-roam. Its
	// schedule kept moving the bot to a resource point while GOTO_FACTORY only
	// asks for a route once the schedule is empty (run 135102: stage-timeout
	// after 40 s, goal-roam arrived 5000 away).
	auto takeMovement = [&]() {
		if (directorOrder)
		{
			m_bEmpiresCommanderMoveFailed = true; // the walk to the factory replaces the director order
			m_bEmpiresCommanderMoveStuck = false;
		}
		if (m_bEmpiresNavGoalActive)
			endEmpiresNavmeshGoal("drop", "vehicle-goto-factory", 0.0f);
		cancelNavigationAttempt("vehicle-goto-factory");
		if (m_pSchedules != nullptr && !m_pSchedules->isEmpty())
			m_pSchedules->freeMemory();
	};

	if (v.bailLogStep > 0 && now >= v.bailLogAt && isAlive())
	{
		const Vector at = getOrigin();
		RCBOT_DIAG("[RCBOT2] vehicle bail-exit userid=%d step=%d pos=(%.0f %.0f %.0f) ground=%d\n", userId, v.bailLogStep, at.x,
			at.y, at.z, GroundAt(at, at.z) ? 1 : 0);
		v.bailLogStep = v.bailLogStep >= 4 ? 0 : v.bailLogStep + 1;
		v.bailLogAt = now + (v.bailLogStep == 2 ? 0.7f : 1.0f);
	}

	if (g_exitBlockUserId >= 0 && inVehicle >= 0 && v.stage == EmpiresVehicleStage::DRIVING &&
		inVehicle == v.vehicleIndex && v.vehicle != nullptr &&
		(g_exitBlockUserId == userId ||
		 (g_exitBlockUserId == 0 && (EmpiresVehicleReadInt(this, v.vehicle, "m_iChassis", -1) == 1 ||
			 EmpiresVehicleReadInt(this, v.vehicle, "m_iChassis", -1) == 9))))
	{
		g_exitBlockUserId = -1;
		edict_t* boxed = engine->PEntityOfEntIndex(inVehicle);
		const int props = ExitBlockSurround(boxed);
		const Vector at = boxed != nullptr ? CBotGlobals::entityOrigin(boxed) : Vector(0, 0, 0);
		// Passenger test: one teammate bot on foot (engineer first, never the commander)
		// respawns, so it lands in the boxed APC's passenger seat.
		int respawned = -1;
		if (g_exitBlockPassenger)
		{
			int fallback = -1;
			for (int i = 1; gpGlobals != nullptr && i <= gpGlobals->maxClients && respawned < 0; ++i)
			{
				edict_t* p = engine->PEntityOfEntIndex(i);
				CBot* bot = p != nullptr && !p->IsFree() && p != m_pEdict ? CBots::getBotPointer(p) : nullptr;
				int commander = 0;
				if (bot == nullptr || CClassInterface::getTeam(p) != team || EmpiresVehiclePlayerVehicle(this, p) >= 0 ||
					(readEmpiresEntityIntProperty(p, "m_bCommander", commander) && commander != 0))
					continue;
				if (static_cast<CBotEmpires*>(bot)->isEmpiresEngineerClass())
					respawned = i;
				else if (fallback < 0)
					fallback = i;
			}
			if (respawned < 0)
				respawned = fallback;
			if (respawned > 0)
			{
				g_exitBlockSpawnUserId = engine->GetPlayerUserId(engine->PEntityOfEntIndex(respawned));
				helpers->ClientCommand(engine->PEntityOfEntIndex(respawned), "kill\n");
			}
		}
		RCBOT_DIAG("[RCBOT2] vehicle exit-block placed userid=%d vehicle=%d chassis=%d props=%d respawn_userid=%d at=(%.0f %.0f %.0f)\n",
			userId, inVehicle, EmpiresVehicleReadInt(this, boxed, "m_iChassis", -1), props,
			respawned > 0 ? engine->GetPlayerUserId(engine->PEntityOfEntIndex(respawned)) : -1, at.x, at.y, at.z);
		if (!g_exitBlockPassenger)
		{
			v.nextAction = now;
			setStage(EmpiresVehicleStage::GIVE_UP_EXIT, 10.0f);
			return;
		}
	}

	// ---------------------------------------------------------------- decide
	if (v.stage == EmpiresVehicleStage::NONE)
	{
		if (inVehicle >= 0 && client > 0 && EmpiresLawlietShouldDisembark(team, client, inVehicle))
		{
			edict_t* seat = engine->PEntityOfEntIndex(inVehicle);
			if (seat != nullptr && IsClass(seat, "emp_vehicle") && EmpiresVehicleAlive(this, seat))
			{
				RCBOT_DIAG("[RCBOT2] lawliet passenger-disembark userid=%d veh=%d\n", userId, inVehicle);
				v.vehicle = seat;
				v.vehicleIndex = inVehicle;
				v.passengerExit = true;
				v.nextAction = now;
				setStage(EmpiresVehicleStage::GIVE_UP_EXIT, 3.0f);
				return;
			}
		}
		if (now < v.nextDecision || now < v.cooldownUntil ||
			(!rcbot_empires_vehicles.GetBool() && !EmpiresIsEngibot(m_pEdict)))
			return;
		v.nextDecision = now + (isEmpiresEngineerClass() ? 0.5f : 2.0f);
		const char* skip = nullptr;
		if (team != 2 && team != 3)
			skip = "team";
		else if (inVehicle >= 0)
		{
			// Driver of an own fighting vehicle without a running stage (run 20261112-182815:
			// spawned into another vehicle than expected, reset "vehicle-changed", then sat
			// idle with "skip in-vehicle"): take it over and drive.
			edict_t* seat = engine->PEntityOfEntIndex(inVehicle);
			int driver = -1;
			if (m_iBuildAssignStage == 0 && (client <= 0 || !EmpiresLawlietBlocksVehicleEntry(team, client)) &&
				seat != nullptr && IsClass(seat, "emp_vehicle") &&
				EmpiresVehicleReadInt(this, seat, "m_iTeamNum", -1) == team &&
				!IsCommanderChassis(EmpiresVehicleReadInt(this, seat, "m_iChassis", -1)) &&
				EmpiresVehicleAlive(this, seat) &&
				readEmpiresEntityHandleProperty(seat, "m_hPassengers", 0, driver) &&
				driver == engine->IndexOfEdict(m_pEdict))
			{
				v.vehicle = seat;
				v.vehicleIndex = inVehicle;
				v.billNye = isEmpiresBillNye();
				RCBOT_DIAG("[RCBOT2] vehicle adopt userid=%d vehicle=%d\n", userId, inVehicle);
				setStage(EmpiresVehicleStage::DRIVING, 0.0f);
				return;
			}
			// Passenger seat of an own vehicle, e.g. after spawning at an APC (TODO 5.23, run
			// 20261121-133146: engineer userid=8 sat in APC 574 for minutes with its build order,
			// origin vehicle-local (90 -60 -17)). A human gets out once the vehicle is slow or
			// has no driver; the commander in its CV is the driver and never reaches this.
			if (seat != nullptr && IsClass(seat, "emp_vehicle") &&
				EmpiresVehicleReadInt(this, seat, "m_iTeamNum", -1) == team &&
				EmpiresVehicleAlive(this, seat) &&
				readEmpiresEntityHandleProperty(seat, "m_hPassengers", 0, driver) &&
				driver != engine->IndexOfEdict(m_pEdict))
			{
				const Vector at = CBotGlobals::entityOrigin(seat);
				const bool sampled = v.passengerSeat == inVehicle && v.lastSampleTime > 0.0f && now > v.lastSampleTime;
				const float speed = sampled ? Dist2D(at, v.lastVehicleOrigin) / (now - v.lastSampleTime) : -1.0f;
				if (v.passengerSeat != inVehicle || v.passengerSince <= 0.0f)
					v.passengerSince = now;
				v.passengerSeat = inVehicle;
				v.lastVehicleOrigin = at;
				v.lastSampleTime = now;
				// A driver that never slows down would carry the passenger for minutes; the exit
				// works while moving, so get out after 10 s anyway.
				const bool seatedLong = now - v.passengerSince > 10.0f;
				const bool lawlietStay = client > 0 && EmpiresLawlietStayInVehicle(team, client, inVehicle);
				if (!lawlietStay && (driver < 0 || (sampled && speed < 150.0f) || seatedLong))
				{
					RCBOT_DIAG("[RCBOT2] vehicle passenger-exit userid=%d vehicle=%d driver=%d speed=%.0f seated=%.0f at=(%.0f %.0f %.0f)\n",
						userId, inVehicle, driver, speed, now - v.passengerSince, at.x, at.y, at.z);
					v.vehicle = seat;
					v.vehicleIndex = inVehicle;
					v.passengerExit = true;
					v.nextAction = now;
					setStage(EmpiresVehicleStage::GIVE_UP_EXIT, 3.0f);
					return;
				}
			}
			skip = "in-vehicle";
		}
		else if (isEmpiresEngineerClass() && !(client > 0 && EmpiresLawlietWantsApcBuy(team, client)))
		{
			skip = "engineer";
			if (!(client > 0 && EmpiresLawlietBlocksSideTasks(team, client)))
			{
				edict_t* hurt = empiresRepairCallAllowed(RepairRequestVehicle(team)) ? RepairRequestVehicle(team) : nullptr;
				RepairRequest& r = s_repairRequest[team];
				// EngiBots also repair damaged own vehicles in the base / on a repair pad without a
				// bot repair call (e.g. human-driven or parked vehicles, TODO 7.1).
				if (hurt == nullptr && RepairRequestVehicle(team) == nullptr && EmpiresIsEngibot(m_pEdict))
				{
					int side = -1;
					bool armorDamaged = false;
					if (edict_t* baseVeh = FindBaseVehicleForEngibotRepair(this, team, side, armorDamaged); baseVeh != nullptr)
					{
						r = RepairRequest();
						r.vehicle = engine->IndexOfEdict(baseVeh);
						r.driverUserId = -1;
						r.origin = CBotGlobals::entityOrigin(baseVeh);
						r.until = now + 20.0f;
						r.side = side;
						r.armorDamaged = armorDamaged;
						r.baseScan = true;
						hurt = baseVeh;
						RCBOT_DIAG("[RCBOT2] engibot vehicle-repair-scan userid=%d veh=%d dist=%.0f side=%d armor=%d\n",
							userId, r.vehicle, Dist2D(getOrigin(), r.origin), side, armorDamaged ? 1 : 0);
					}
				}
				if (hurt != nullptr && (r.engineerUserId == userId || now > r.claimUntil) &&
					(NearestOnVehicle(hurt, getEyePosition()) - getEyePosition()).Length2D() < 220.0f)
				{
					r.engineerUserId = userId;
					r.claimUntil = now + 10.0f;
					v.assistVehicle = hurt;
					v.vehicleIndex = engine->IndexOfEdict(hurt);
					setStage(EmpiresVehicleStage::REPAIR_ASSIST, 60.0f);
					return;
				}
				// Further away: interrupt whatever schedule runs (goal-roam does
				// not return to getTasks for minutes, run 214021) and walk there.
				if (hurt != nullptr && handleEmpiresVehicleRepairRequest())
					return;
			}
		}
		if (skip != nullptr)
		{
			if (now >= v.nextLog)
			{
				v.nextLog = now + 20.0f;
				RCBOT_DIAG("[RCBOT2] vehicle skip userid=%d reason=%s vehicle=%d\n", userId, skip, inVehicle);
			}
			return;
		}
		if (client > 0 && EmpiresLawlietBlocksVehicleEntry(team, client))
			return;

		const bool billNye = isEmpiresBillNye();
		// Do not hijack a bot currently walking to open a map door/gate button
		// (as_escort: vehicle goto-factory fought over navigation for 90 s).
		if (m_iEmpiresDoorButton >= 0)
			return;
		const bool lawlietBuyer = client > 0 && EmpiresLawlietWantsApcBuy(team, client);
		if (!billNye && !lawlietBuyer)
		{
			int drivers = 0;
			int teamBots = 1;
			for (int slot = 0; slot < RCBOT_MAXPLAYERS; ++slot)
			{
				CBot* bot = CBots::getBot(slot);
				if (bot == nullptr || bot == this || !bot->inUse() || bot->getEdict() == nullptr ||
					CClassInterface::getTeam(bot->getEdict()) != team)
					continue;
				const CBotEmpires* other = static_cast<const CBotEmpires*>(bot);
				++teamBots;
				if (other->m_vehicle.stage != EmpiresVehicleStage::NONE && !other->m_vehicle.billNye)
					++drivers;
			}
			// 11 vs 11 on rainbowroad with a limit of 2: most bots walked (user).
			const int limit = rcbot_empires_vehicle_drivers.GetInt() >= 0 ? rcbot_empires_vehicle_drivers.GetInt() :
				(std::max)(2, (teamBots * 2 + 2) / 5);
			if (drivers >= limit)
				return;
		}
		if (rcbot_empires_vehicle_debug_team.GetInt() > 0 && team != rcbot_empires_vehicle_debug_team.GetInt())
			return;

		// An empty own vehicle close by is used before buying a new one:
		// (A) blocking our own VF bay (3.17): adopt after 5 s if no human is right there;
		// (B) parked at the own base (3.10e): adopt after 30 s;
		// (C) abandoned by another bot in the field (3.26 3b): another bot tries once after 10 s.
		constexpr float kBaseRadius = 1500.0f;
		constexpr float kEmptyFor = 30.0f;
		constexpr float kBayEmptyFor = 5.0f;
		constexpr float kAbandonedEmptyFor = 10.0f;
		edict_t* ownFactory = nullptr;
		Vector ownFactoryOrigin;
		int ownFactoryTeam = -1;
		if (!billNye)
			findEmpiresVehicleFactory(team, ownFactory, ownFactoryOrigin, ownFactoryTeam);
		std::vector<Vector> basePoints;
		for (int i = gpGlobals->maxClients + 1; i < gpGlobals->maxEntities && !billNye; ++i)
		{
			edict_t* e = engine->PEntityOfEntIndex(i);
			if (e == nullptr || e->IsFree() || e->GetClassName() == nullptr)
				continue;
			const bool building = std::strncmp(e->GetClassName(), "emp_building_", 13) == 0;
			if ((building || (IsClass(e, "emp_vehicle") && IsCommanderChassis(EmpiresVehicleReadInt(this, e, "m_iChassis", -1)))) &&
				EmpiresVehicleReadInt(this, e, "m_iTeamNum", -1) == team)
				basePoints.push_back(CBotGlobals::entityOrigin(e));
		}
		edict_t* best = nullptr;
		const char* bestAdoptReason = nullptr;
		float bestEmptyFor = 0.0f;
		float bestDistance = billNye ? 0.0f : 2500.0f;
		for (int i = gpGlobals->maxClients + 1; i < gpGlobals->maxEntities && !billNye; ++i)
		{
			edict_t* e = engine->PEntityOfEntIndex(i);
			if (!IsClass(e, "emp_vehicle") || EmpiresVehicleReadInt(this, e, "m_iTeamNum", -1) != team ||
				IsCommanderChassis(EmpiresVehicleReadInt(this, e, "m_iChassis", -1)) || !EmpiresVehicleAlive(this, e))
				continue;
			const int ch = EmpiresVehicleReadInt(this, e, "m_iChassis", -1);
			if (lawlietBuyer && ch != 1 && ch != 9)
				continue;
			int driver = -1;
			if (!readEmpiresEntityHandleProperty(e, "m_hPassengers", 0, driver))
				continue;
			const float emptyFor = UpdateVehicleEmptyTime(e, i, driver, now);
			const Vector at = CBotGlobals::entityOrigin(e);
			float baseDistance = 1.0e9f;
			for (const Vector& p : basePoints)
				baseDistance = (std::min)(baseDistance, (p - at).Length2D());
			const bool inOwnBay = ownFactory != nullptr && IsNearFactoryBay(ownFactory, at);
			const BotAbandonRecord& rec = g_botAbandon[i];
			const bool botAbandoned = rec.serial == e->m_NetworkSerialNumber && rec.abandonedAt > 0.0f &&
				(now - rec.abandonedAt) < 180.0f && rec.abandonCount == 1;
			const bool wedged = rec.serial == e->m_NetworkSerialNumber && rec.abandonCount >= 2;
			const bool bayEligible = inOwnBay && emptyFor >= kBayEmptyFor &&
				!HasNearbyHumanPlayer(team, at, 250.0f);
			// The factory is itself a base point. A bay vehicle must still pass the nearby-human check.
			const bool baseEligible = !inOwnBay && !wedged && baseDistance < kBaseRadius && emptyFor >= kEmptyFor;
			const bool abandonedEligible = botAbandoned && rec.lastDriverUserId != userId &&
				emptyFor >= kAbandonedEmptyFor && !HasNearbyHumanPlayer(team, at, 400.0f);
			if (driver >= 0 || (!bayEligible && !baseEligible && !abandonedEligible))
			{
				if (now >= v.nextEmptyLog)
				{
					v.nextEmptyLog = now + 10.0f;
					RCBOT_DIAG("[RCBOT2] vehicle empty-skip userid=%d vehicle=%d driver=%d empty_for=%.0f base_dist=%.0f dist=%.0f\n", userId,
						i, driver, emptyFor, baseDistance, distanceFrom(at));
				}
				continue;
			}
			// Another bot may already be walking to it; this bot's own abandon cooldown still applies
			// except when clearing a blocked factory bay.
			bool claimed = !bayEligible && (v.abandonedVehicle == i && now < v.abandonedUntil);
			for (int slot = 0; slot < RCBOT_MAXPLAYERS && !claimed; ++slot)
			{
				CBot* bot = CBots::getBot(slot);
				if (bot == nullptr || bot == this || !bot->inUse())
					continue;
				const EmpiresVehicleBrain& other = static_cast<const CBotEmpires*>(bot)->m_vehicle;
				claimed = (other.vehicleIndex == i);
			}
			const float distance = distanceFrom(at);
			if (!claimed && distance < bestDistance)
			{
				bestDistance = distance;
				best = e;
				bestEmptyFor = emptyFor;
				bestAdoptReason = bayEligible ? "adopt-bay-blocker" :
					(abandonedEligible && !baseEligible ? "adopt-abandoned" : nullptr);
			}
		}

		if (now >= v.nextEmptyLog)
			v.nextEmptyLog = now + 10.0f;
		v.billNye = billNye;
		if (best != nullptr)
		{
			v.vehicle = best;
			v.vehicleIndex = engine->IndexOfEdict(best);
			if (bestAdoptReason != nullptr)
				RCBOT_DIAG("[RCBOT2] stall-escalate who=vehicle userid=%d stage=1 reason=%s veh=%d empty_for=%.0f dist=%.0f\n",
					userId, bestAdoptReason, v.vehicleIndex, bestEmptyFor, bestDistance);
			setStage(EmpiresVehicleStage::APPROACH, 40.0f);
			return;
		}

		// Infantry opening: no purchase in the first rcbot_empires_vehicle_vf_grace seconds after the
		// team's first finished factory, unless an enemy vehicle is known (team director).
		if (const float graceLeft = EmpiresTeamDirectorVehicleGraceLeft(team); graceLeft > 0.0f)
		{
			if (now >= v.nextLog)
			{
				v.nextLog = now + 20.0f;
				RCBOT_DIAG("[RCBOT2] vehicle skip userid=%d reason=vf-grace left=%.0f\n", userId, graceLeft);
			}
			return;
		}
		edict_t* factory = nullptr;
		Vector factoryOrigin;
		int factoryTeam = -1;
		if (!(rcbot_empires_vehicle_debug_buy.GetBool() && DebugBuildVehicles()) &&
			findEmpiresVehicleFactory(team, factory, factoryOrigin, factoryTeam))
		{
			// TODO 5.5a: a purchase the buyer's wages do not cover comes from the team pool;
			// leave the Commander's reserve for its next building/research in it.
			float pool = -1.0f;
			EmpiresTeamResources(team, pool);
			const float wages = EmpiresPlayerWages(m_pEdict);
			const EmpiresTeamEconomy::TeamState& money = EmpiresTeamEconomy::Team(team);
			const float reserve = EmpiresTeamEconomy::ActiveReserve(money, gpGlobals->curtime);
			if (!lawlietBuyer && !EmpiresTeamEconomy::PurchaseAllowed(wages, pool, money.vehicleCost, reserve))
			{
				if (now >= v.nextLog)
				{
					v.nextLog = now + 20.0f;
					RCBOT_DIAG("[RCBOT2] vehicle skip userid=%d reason=team-reserve wages=%.0f pool=%.0f cost=%.0f reserve=%.0f\n",
						userId, wages, pool, money.vehicleCost, reserve);
				}
				return;
			}
			const float lawlietApcMinCost = team == 2 ? 324.0f : 364.0f;
			if (lawlietBuyer && pool >= 0.0f && wages + pool < lawlietApcMinCost)
			{
				if (now >= v.nextLog)
				{
					v.nextLog = now + 20.0f;
					RCBOT_DIAG("[RCBOT2] vehicle skip userid=%d reason=lawliet-funds wages=%.0f pool=%.0f need=%.0f\n",
						userId, wages, pool, lawlietApcMinCost);
				}
				return;
			}
			// Buyer with the highest wages first: a free, alive teammate (no stage, not in a
			// vehicle, not an engineer, not cooling down) with more wages buys instead.
			float richest = -1.0f;
			int richestUser = -1;
			for (int slot = 0; slot < RCBOT_MAXPLAYERS; ++slot)
			{
				CBot* bot = CBots::getBot(slot);
				if (bot == nullptr || bot == this || !bot->inUse() || bot->getEdict() == nullptr || !bot->isAlive() ||
					CClassInterface::getTeam(bot->getEdict()) != team)
					continue;
				CBotEmpires* other = static_cast<CBotEmpires*>(bot);
				// Only bots that reached this decision themselves in the last seconds (run
				// 20261114: the richer bot sat in a commander stage and never bought).
				if (other->m_vehicle.stage != EmpiresVehicleStage::NONE || now < other->m_vehicle.cooldownUntil ||
					other->m_vehicle.nextDecision < now - 1.0f || other->commanderDutyBlocksSideTasks() ||
					other->isEmpiresEngineerClass() || EmpiresVehiclePlayerVehicle(other, other->getEdict()) >= 0)
					continue;
				const float w = EmpiresPlayerWages(other->getEdict());
				if (w > richest)
				{
					richest = w;
					richestUser = engine->GetPlayerUserId(other->getEdict());
				}
			}
			if (v.deferredSince <= 0.0f || now - v.deferredSince > 60.0f) // stale wait from an earlier try
				v.deferredSince = now;
			if (!lawlietBuyer &&
				EmpiresTeamEconomy::DeferToRicherBuyer(wages, richest, money.vehicleCost, now - v.deferredSince, 30.0f))
			{
				if (now >= v.nextLog)
				{
					v.nextLog = now + 20.0f;
					RCBOT_DIAG("[RCBOT2] vehicle skip userid=%d reason=richer-buyer wages=%.0f other=%d other_wages=%.0f\n",
						userId, wages, richestUser, richest);
				}
				return;
			}
			v.deferredSince = 0.0f;
			v.factory = factory;
			v.factoryIndex = engine->IndexOfEdict(factory);
			v.buyInPlace = false;
			v.factoryRoute = FactoryRoute(factory, getOrigin());
			v.factoryRouteIndex = 0;
			v.goal = v.factoryRoute.front();
			v.purchaseStep = 0;
			takeMovement();
			// Walk time grows with the distance (Coast, run 20261120-163321: buyers at the centre flag,
			// ~11000 from the factory, timed out after a fixed 90 s again and again; 1 tank in 10 min).
			const float walkLimit = (std::max)(90.0f, (std::min)(300.0f, Dist2D(getOrigin(),
				CBotGlobals::entityOrigin(factory)) / 100.0f + 30.0f));
			setStage(EmpiresVehicleStage::GOTO_FACTORY, walkLimit);
			v.walkFrom = getOrigin();
			v.walkCheckAt = now + 30.0f;
			v.walkRequests = 0;
			v.walkBestFactory = Dist2D(getOrigin(), CBotGlobals::entityOrigin(factory));
			v.walkBestAt = now;
			v.walkAwayReplans = 0;
			RCBOT_DIAG("[RCBOT2] vehicle factory-route userid=%d points=%zu first=(%.0f %.0f)\n", userId,
				v.factoryRoute.size(), v.goal.x, v.goal.y);
			if (RCBotNavMesh::IsLoaded())
				requestNavigation(v.goal, RCBotNavigation::NavigationPolicy::NAVMESH_ONLY, 0);
			return;
		}
		if (now >= v.nextLog)
		{
			v.nextLog = now + 20.0f;
			RCBOT_DIAG("[RCBOT2] vehicle skip userid=%d reason=%s\n", userId,
				DebugBuildVehicles() && rcbot_empires_vehicle_debug_buy.GetBool() ? "debug-buy" : "no-factory");
		}
		// In-place debug buying only for tests (rcbot_empires_vehicle_debug_buy 1): the test
		// server runs emp_sv_debug_build_vehicles 1, and bots dropped vehicles on the spot
		// before a factory stood (user: "bots spawn in vehicles without buying at the VF").
		if (DebugBuildVehicles() && rcbot_empires_vehicle_debug_buy.GetBool())
		{
			// The debug build drops the vehicle 200 above the buyer: find open
			// road first, preferring the direction of the enemy.
			const Vector me = getOrigin();
			Vector enemy = me + Vector(1000, 0, 0);
			EnemyCommanderOrigin(enemyTeam, enemy);
			const float toEnemy = YawTo(me, enemy);
			float bestScore = 1.0e9f;
			Vector bestSpot = me;
			for (float radius = 0.0f; radius <= 1600.0f; radius += 400.0f)
				for (int k = 0; k < (radius == 0.0f ? 1 : 16); ++k)
				{
					const float yaw = toEnemy + k * 22.5f;
					const Vector spot = me + YawDir(yaw) * radius;
					if (!VehicleSpotFree(spot))
						continue;
					// Only a spot the buyer can walk to (emp_coast centre bunker: three 40 s timeouts).
					Vector mesh;
					if (radius > 0.0f && RCBotNavMesh::IsLoaded() &&
						(!RCBotNavMesh::ClosestPointOnMesh(spot, 120.0f, mesh) || !RCBotNavMesh::CanReachComponent(me, mesh)))
						continue;
					const float score = radius + std::fabs(NormalizeYaw(yaw - toEnemy)) * 4.0f;
					if (score < bestScore)
					{
						bestScore = score;
						bestSpot = spot;
					}
				}
			if (bestScore >= 1.0e9f)
			{
				if (now >= v.nextLog)
				{
					v.nextLog = now + 20.0f;
					RCBOT_DIAG("[RCBOT2] vehicle skip userid=%d reason=no-free-spot\n", userId);
				}
				return;
			}
			v.buyInPlace = true;
			v.factoryIndex = -1;
			v.goal = bestSpot;
			v.nextAction = now;
			RCBOT_DIAG("[RCBOT2] vehicle buy-spot userid=%d spot=(%.0f %.0f %.0f) dist=%.0f\n", userId,
				bestSpot.x, bestSpot.y, bestSpot.z, distanceFrom(bestSpot));
			takeMovement();
			setStage(EmpiresVehicleStage::GOTO_FACTORY, 40.0f);
		}
		return;
	}

	edict_t* vehicle = v.vehicle.get();
	// A buyer that keeps walking keeps its deadline (Palmbay run 20261120-182057: routes to the plateau
	// factory wind around a cliff ring, 37-77 areas; buyers timed out on the way, then roamed off).
	// Moved distance, not distance to the factory: the detour first leads away from it.
	if (v.stage == EmpiresVehicleStage::GOTO_FACTORY && !v.buyInPlace && now >= v.walkCheckAt)
	{
		// A buyer that has been walking away from the factory for 60 s (1500 farther than its best) is not
		// on a detour (R3 gap, Palmbay 20261125-161515: one buyer in 4 min with 5000 resources): replan the
		// factory route once, then give the walk up instead of extending its deadline.
		edict_t* walkFactory = v.factory.get();
		const float factoryDist = walkFactory != nullptr && !walkFactory->IsFree() ?
			Dist2D(getOrigin(), CBotGlobals::entityOrigin(walkFactory)) : -1.0f;
		bool movingAway = false;
		if (factoryDist >= 0.0f)
		{
			if (factoryDist < v.walkBestFactory)
			{
				v.walkBestFactory = factoryDist;
				v.walkBestAt = now;
			}
			else if (factoryDist > v.walkBestFactory + 1500.0f && now - v.walkBestAt >= 60.0f)
				movingAway = true;
		}
		if (movingAway)
		{
			RCBOT_DIAG("[RCBOT2] stall-escalate who=buyer stage=%d reason=moving-away userid=%d factory_dist=%.0f best=%.0f for=%.0fs\n",
				v.walkAwayReplans + 1, userId, factoryDist, v.walkBestFactory, now - v.walkBestAt);
			if (v.walkAwayReplans >= 1)
			{
				resetEmpiresVehicle("moving-away", 20.0f);
				return;
			}
			++v.walkAwayReplans;
			v.walkBestFactory = factoryDist;
			v.walkBestAt = now;
			cancelNavigationAttempt("buyer-moving-away");
			if (m_pSchedules != nullptr)
				m_pSchedules->freeMemory();
			if (m_pNavigator != nullptr)
				m_pNavigator->clear();
			v.factoryRoute = FactoryRoute(walkFactory, getOrigin());
			v.factoryRouteIndex = 0;
			if (!v.factoryRoute.empty())
				v.goal = v.factoryRoute.front();
			if (RCBotNavMesh::IsLoaded())
				requestNavigation(v.goal, RCBotNavigation::NavigationPolicy::NAVMESH_ONLY, 0);
		}
		else if (Dist2D(getOrigin(), v.walkFrom) >= 300.0f && now - v.stageSince < 600.0f)
			v.stageUntil = (std::max)(v.stageUntil, now + 30.0f);
		v.walkFrom = getOrigin();
		v.walkCheckAt = now + 30.0f;
	}
	// Keep trying to leave the same vehicle even after the deadline. Resetting
	// while still seated leaves the bot unable to plan or exit (run 125334).
	if (v.stageUntil > 0.0f && now > v.stageUntil && v.stage != EmpiresVehicleStage::GIVE_UP_EXIT)
	{
		++v.failures;
		if (v.failures >= 2 && !v.billNye && !(client > 0 && EmpiresLawlietWantsApcBuy(team, client)))
			v.wantJeep = true;
		if (v.stage == EmpiresVehicleStage::GOTO_FACTORY)
			RCBOT_DIAG("[RCBOT2] vehicle walk-timeout userid=%d pos=(%.0f %.0f %.0f) goal=(%.0f %.0f) step=%d route=%zu/%zu "
				"moved30=%.0f elapsed=%.0f sched_empty=%d requests=%d enemy=%d\n", userId, getOrigin().x, getOrigin().y, getOrigin().z, v.goal.x,
				v.goal.y, v.purchaseStep, v.factoryRouteIndex, v.factoryRoute.size(), Dist2D(getOrigin(), v.walkFrom), now - v.stageSince,
				m_pSchedules != nullptr && m_pSchedules->isEmpty() ? 1 : 0, v.walkRequests, hasEnemy() ? 1 : 0);
		resetEmpiresVehicle("stage-timeout", 10.0f * (std::min)(v.failures, 6));
		return;
	}

	// ------------------------------------------------------- walk to factory
	if (v.stage == EmpiresVehicleStage::GOTO_FACTORY)
	{
		edict_t* factory = v.factory.get();
		if (!v.buyInPlace && (factory == nullptr || factory->IsFree()))
		{
			resetEmpiresVehicle("factory-gone", 10.0f);
			return;
		}
		Vector origin = v.goal;
		bool ready = false;
		if (v.buyInPlace)
			ready = distanceFrom(origin) < 90.0f;
		else
		{
			// 3.17 / 3.26 (3b): once at the ramp entrance, if an empty own vehicle is blocking
			// the factory bay/ramp, adopt and drive it out instead of hitting #emp_vehicle_blocked.
			if (v.purchaseStep >= 1)
			{
				for (int i = gpGlobals->maxClients + 1; i < gpGlobals->maxEntities; ++i)
				{
					edict_t* e = engine->PEntityOfEntIndex(i);
					if (!IsClass(e, "emp_vehicle") || EmpiresVehicleReadInt(this, e, "m_iTeamNum", -1) != team ||
						IsCommanderChassis(EmpiresVehicleReadInt(this, e, "m_iChassis", -1)) ||
						!EmpiresVehicleAlive(this, e))
						continue;
					const Vector at = CBotGlobals::entityOrigin(e);
					if (!IsNearFactoryBay(factory, at))
						continue;
					int driver = -1;
					if (!readEmpiresEntityHandleProperty(e, "m_hPassengers", 0, driver))
						continue;
					const float emptyFor = UpdateVehicleEmptyTime(e, i, driver, now);
					if (driver >= 0 || emptyFor < 5.0f || HasNearbyHumanPlayer(team, at, 250.0f))
						continue;
					bool claimed = false;
					for (int slot = 0; slot < RCBOT_MAXPLAYERS && !claimed; ++slot)
					{
						CBot* bot = CBots::getBot(slot);
						if (bot != nullptr && bot != this && bot->inUse())
							claimed = static_cast<const CBotEmpires*>(bot)->m_vehicle.vehicleIndex == i;
					}
					if (claimed)
						continue;
					v.vehicle = e;
					v.vehicleIndex = i;
					RCBOT_DIAG("[RCBOT2] stall-escalate who=buyer userid=%d stage=1 reason=bay-occupied veh=%d empty_for=%.0f\n",
						userId, i, emptyFor);
					setStage(EmpiresVehicleStage::APPROACH, 40.0f);
					return;
				}
			}
			// Walk to the ramp, then straight up into the bay; the game only
			// sells to a player standing on the factory floor.
			Vector entrance, inside;
			FactoryPoints(factory, entrance, inside);
			ready = v.purchaseStep >= 1 && StandingInFactory(m_pEdict, factory);
			if (!ready && v.purchaseStep == 1 && Dist2D(getOrigin(), inside) < 120.0f)
			{
				int candidates = 0;
				Vector stand;
				char why[320] = "";
				if (FindFactoryStandSpot(factory, getOrigin(), stand, candidates, why, sizeof(why)))
				{
					v.purchaseStep = 2;
					v.goal = stand;
					RCBOT_DIAG("[RCBOT2] vehicle factory-stand userid=%d spot=(%.0f %.0f %.0f) candidates=%d\n",
						userId, stand.x, stand.y, stand.z, candidates);
				}
				else if (now >= v.nextLog)
				{
					v.nextLog = now + 5.0f;
					RCBOT_DIAG("[RCBOT2] vehicle factory-stand userid=%d none pos=(%.0f %.0f %.0f) %s\n", userId,
						getOrigin().x, getOrigin().y, getOrigin().z, why);
				}
			}
			if (!ready && v.purchaseStep == 2)
			{
				setMoveTo(v.goal);
				return;
			}
			// Corner waypoints before the ramp.
			if (v.purchaseStep == 0 && v.factoryRouteIndex + 1 < v.factoryRoute.size() &&
				Dist2D(getOrigin(), v.factoryRoute[v.factoryRouteIndex]) < 200.0f)
			{
				++v.factoryRouteIndex;
				v.goal = v.factoryRoute[v.factoryRouteIndex];
				v.nextAction = now;
				RCBOT_DIAG("[RCBOT2] vehicle factory-route userid=%d next=%zu at=(%.0f %.0f)\n", userId,
					v.factoryRouteIndex, v.goal.x, v.goal.y);
				if (RCBotNavMesh::IsLoaded())
					requestNavigation(v.goal, RCBotNavigation::NavigationPolicy::NAVMESH_ONLY, 0);
				return;
			}
			if (!ready && (v.purchaseStep == 1 || distanceFrom(entrance) < 200.0f))
			{
				if (v.purchaseStep != 1)
					RCBOT_DIAG("[RCBOT2] vehicle factory-enter userid=%d entrance=(%.0f %.0f) inside=(%.0f %.0f)\n",
						userId, entrance.x, entrance.y, inside.x, inside.y);
				v.purchaseStep = 1; // inside the approach: no more navmesh
				setMoveTo(inside);
				return;
			}
			origin = v.factoryRoute.empty() ? entrance : v.factoryRoute[v.factoryRouteIndex];
		}
		if (ready)
		{
			if (!v.buyInPlace)
			{
				bool solid = false;
				float fraction = 1.0f;
				edict_t* hitEdict = GameFactoryTraceHit(getOrigin(), &solid, &fraction);
				RCBOT_DIAG("[RCBOT2] vehicle in-factory userid=%d pos=(%.0f %.0f %.0f) game-trace hit=%s\n", userId,
					getOrigin().x, getOrigin().y, getOrigin().z,
					hitEdict != nullptr && hitEdict->GetClassName() != nullptr ? hitEdict->GetClassName() : "none");
			}
			stopMoving();
			v.purchaseStep = 0;
			v.nextAction = now;
			setStage(EmpiresVehicleStage::BUYING, 10.0f);
			return;
		}
		if (!RCBotNavMesh::IsLoaded() || (m_pSchedules != nullptr && m_pSchedules->isEmpty()))
		{
			// The path ended as close to a corner as the mesh allows (corner on a plateau edge,
			// Palmbay runs 20261120-211159/211224: buyers stopped ~310 from it, the 200 check never
			// passed): take the next corner instead of asking for the same path again.
			if (v.purchaseStep == 0 && v.factoryRouteIndex + 1 < v.factoryRoute.size() &&
				Dist2D(getOrigin(), v.factoryRoute[v.factoryRouteIndex]) < 500.0f && v.walkRequests >= 1)
			{
				RCBOT_DIAG("[RCBOT2] stall-escalate who=buyer userid=%d stage=1 reason=corner-unreachable corner=(%.0f %.0f) dist=%.0f\n",
					userId, v.factoryRoute[v.factoryRouteIndex].x, v.factoryRoute[v.factoryRouteIndex].y,
					Dist2D(getOrigin(), v.factoryRoute[v.factoryRouteIndex]));
				++v.factoryRouteIndex;
				v.goal = v.factoryRoute[v.factoryRouteIndex];
				v.nextAction = now;
				v.walkRequests = 0;
				requestNavigation(v.goal, RCBotNavigation::NavigationPolicy::NAVMESH_ONLY, 0);
				return;
			}
			if (RCBotNavMesh::IsLoaded() && now >= v.nextAction)
			{
				v.nextAction = now + 4.0f;
				// A hang blocks goal and segment for 15 s, so every re-request failed at once and the
				// buyer stood still (Palmbay runs 20261120-1834xx: 13-22 requests in 90 s, moved 0, no
				// enemy). A fresh route from where the bot stands usually works (as build-assign does).
				clearNavigationAttemptCooldownForStartAnchorRetry();
				requestNavigation(origin, RCBotNavigation::NavigationPolicy::NAVMESH_ONLY, 0);
				++v.walkRequests;
			}
			else if (!RCBotNavMesh::IsLoaded())
				setMoveTo(origin);
		}
		return;
	}

	// ------------------------------------------------------------ purchase
	if (v.stage == EmpiresVehicleStage::BUYING)
	{
		Vector enemy;
		v.view = QAngle(0, m_vViewAngles.y, 0);
		if (EnemyCommanderOrigin(enemyTeam, enemy))
			v.view.y = YawTo(getOrigin(), enemy);
		if (now < v.nextAction)
			return;
		v.nextAction = now + 0.15f;
		const bool lawlietBuyer = client > 0 && EmpiresLawlietWantsApcBuy(team, client);
		const bool jeep = !lawlietBuyer && (v.billNye || v.wantJeep);
		const bool nf = team == 2;
		char command[96] = {};
		switch (v.purchaseStep)
		{
		case 0:
		{
			v.goal = getOrigin();
			v.nextLog = 0.0f;
			v.baseline.clear();
			for (int i = gpGlobals->maxClients + 1; i < gpGlobals->maxEntities; ++i)
				if (IsClass(engine->PEntityOfEntIndex(i), "emp_vehicle"))
					v.baseline.push_back(i);
			if (v.buyInPlace)
			{
				// The debug build spawns the vehicle 200 above the player, facing
				// the player's view: look toward the enemy side first.
				m_vViewAngles.x = 0.0f;
			}
			bool researchDone[EmpiresCommanderEconomy::kResearchItemCount] = {};
			const bool haveResearch = EmpiresReadTeamResearch(team, researchDone);
			float budget = -1.0f;
			if (!v.buyInPlace)
			{
				float pool = -1.0f;
				if (EmpiresTeamResources(team, pool))
				{
					const float wages = (std::max)(0.0f, EmpiresPlayerWages(m_pEdict));
					const float reserve = lawlietBuyer ? 0.0f :
						EmpiresTeamEconomy::ActiveReserve(
							EmpiresTeamEconomy::Team(team), gpGlobals->curtime);
					budget = wages + (std::max)(0.0f, pool - reserve);
				}
			}
			bool wantApc = lawlietBuyer;
			if (!lawlietBuyer && !jeep && !v.buyInPlace && haveResearch &&
				researchDone[EmpiresCommanderEconomy::kResearchApcSpawn])
			{
				bool hasApc = false;
				const int maxEnts = engine->GetEntityCount();
				for (int eIdx = gpGlobals->maxClients + 1; eIdx < maxEnts; ++eIdx)
				{
					edict_t* ent = engine->PEntityOfEntIndex(eIdx);
					if (ent == nullptr || ent->IsFree() || !IsClass(ent, "emp_vehicle") ||
						CClassInterface::getTeam(ent) != team || !EmpiresVehicleAlive(this, ent))
						continue;
					const int ch = EmpiresVehicleReadInt(this, ent, "m_iChassis", -1);
					if (ch == 1 || ch == 9)
					{
						hasApc = true;
						break;
					}
				}
				wantApc = !hasApc;
			}
			v.purchaseLoadout = EmpiresVehicleLoadout::ChoosePurchaseLoadout(
				nf, jeep, haveResearch ? researchDone : nullptr, budget, wantApc,
				rcbot_empires_vehicle_missiles.GetBool());
			if (v.purchaseLoadout.isApc())
				RCBOT_DIAG("[RCBOT2] vehicle apc-buy userid=%d team=%d chassis=%d cost=%d\n",
					userId, team, v.purchaseLoadout.chassis, v.purchaseLoadout.estimatedCost);
			snprintf(command, sizeof(command), "emp_vehicle_chassis %d\n", v.purchaseLoadout.chassis);
			break;
		}
		case 1:
			if (!v.purchaseLoadout.isJeep() && v.purchaseLoadout.cannonSlot >= 0)
				snprintf(command, sizeof(command), "emp_vehicle_weapon %d 0 %d 1 %d 0 0 0 0 1\n",
					v.purchaseLoadout.cannonSlot, v.purchaseLoadout.cannonWeapon,
					v.purchaseLoadout.missileSlot >= 0 ? 1 : 0);
			break;
		case 2:
			if (!v.purchaseLoadout.isJeep() && v.purchaseLoadout.mgSlot >= 0)
				snprintf(command, sizeof(command), "emp_vehicle_weapon %d 0 %d 1 %d 0 0 0 0 %d\n",
					v.purchaseLoadout.mgSlot, v.purchaseLoadout.mgWeapon,
					v.purchaseLoadout.missileSlot >= 0 ? 1 : 0,
					rcbot_empires_vehicle_debug_mixed.GetBool() ? 1 : 0);
			// 3.10f: the missile launcher only in fire group 1, on the cannon's button.
			if (!v.purchaseLoadout.isJeep() && v.purchaseLoadout.missileSlot >= 0 && helpers != nullptr)
			{
				char missile[96];
				snprintf(missile, sizeof(missile), "emp_vehicle_weapon %d 0 %d 1 0 0 0 0 0 1\n",
					v.purchaseLoadout.missileSlot, v.purchaseLoadout.missileWeapon);
				helpers->ClientCommand(m_pEdict, missile);
				RCBOT_DIAG("[RCBOT2] vehicle buy userid=%d step=%d cmd=%s", userId, v.purchaseStep, missile);
			}
			break;
		case 3:
			snprintf(command, sizeof(command), "emp_vehicle_armor 0 %d\n", v.purchaseLoadout.armorType);
			break;
		case 4:
			snprintf(command, sizeof(command), "emp_vehicle_armor_levels %d %d %d %d\n",
				v.purchaseLoadout.armorLevels[0], v.purchaseLoadout.armorLevels[1],
				v.purchaseLoadout.armorLevels[2], v.purchaseLoadout.armorLevels[3]);
			break;
		case 5:
			snprintf(command, sizeof(command), "emp_vehicle_engine 0 %d\n", v.purchaseLoadout.engine);
			break;
		case 6:
			if (!v.buyInPlace)
				snprintf(command, sizeof(command), "emp_vehicle_factory %d\n", v.factoryIndex);
			break;
		case 7:
			// Diagnostic: production purchases never spawned a vehicle (run 103144).
			LogEmpiresFactoryProbe("buy", userId, m_pEdict, v.factoryIndex);
			LogEmpiresVehicleEcon("before", userId, getTeam(), m_pEdict);
			v.factoryProbeAt = now + 1.0f;
			snprintf(command, sizeof(command), "emp_vehicle_build\n");
			break;
		default:
			setStage(EmpiresVehicleStage::WAIT_SPAWN, 5.0f);
			return;
		}
		if (command[0] != '\0' && helpers != nullptr)
		{
			helpers->ClientCommand(m_pEdict, command);
			RCBOT_DIAG("[RCBOT2] vehicle buy userid=%d step=%d cmd=%s", userId, v.purchaseStep, command);
		}
		++v.purchaseStep;
		return;
	}

	if (v.stage == EmpiresVehicleStage::WAIT_SPAWN)
	{
		if (v.factoryProbeAt > 0.0f && now >= v.factoryProbeAt)
		{
			v.factoryProbeAt = 0.0f;
			LogEmpiresFactoryProbe("wait", userId, m_pEdict, v.factoryIndex);
		}
		// The debug build seats the buyer at once (player origin then reads 0).
		if (inVehicle >= 0)
		{
			edict_t* seated = engine->PEntityOfEntIndex(inVehicle);
			v.vehicle = seated;
			v.vehicleIndex = inVehicle;
			v.failures = 0;
			const Vector at = CBotGlobals::entityOrigin(seated);
			LogEmpiresVehicleEcon("after", userId, getTeam(), m_pEdict);
			v.loadoutVerify = true; // log the bought loadout once (3.10f)
			RCBOT_DIAG("[RCBOT2] vehicle bought userid=%d vehicle=%d chassis=%d pos=(%.0f %.0f %.0f) seated=1\n",
				userId, inVehicle, EmpiresVehicleReadInt(this, seated, "m_iChassis", -1), at.x, at.y, at.z);
			v.lastVehicleOrigin = at;
			v.lastSampleTime = now;
			setStage(EmpiresVehicleStage::DRIVING, 0.0f);
			return;
		}
		edict_t* found = nullptr;
		float bestDistance = 3000.0f;
		for (int i = gpGlobals->maxClients + 1; i < gpGlobals->maxEntities; ++i)
		{
			edict_t* e = engine->PEntityOfEntIndex(i);
			if (!IsClass(e, "emp_vehicle") ||
				std::find(v.baseline.begin(), v.baseline.end(), i) != v.baseline.end() ||
				EmpiresVehicleReadInt(this, e, "m_iTeamNum", team) != team)
				continue;
			int driver = -1;
			if (!readEmpiresEntityHandleProperty(e, "m_hPassengers", 0, driver) || driver >= 0)
				continue;
			bool claimed = false;
			for (int slot = 0; slot < RCBOT_MAXPLAYERS && !claimed; ++slot)
			{
				CBot* bot = CBots::getBot(slot);
				if (bot != nullptr && bot != this && bot->inUse())
					claimed = static_cast<const CBotEmpires*>(bot)->m_vehicle.vehicleIndex == i;
			}
			if (claimed)
				continue;
			const float distance = (CBotGlobals::entityOrigin(e) - v.goal).Length();
			if (distance < bestDistance)
			{
				bestDistance = distance;
				found = e;
			}
		}
		if (found == nullptr)
		{
			if (now >= v.nextLog)
			{
				v.nextLog = now + 1.0f;
				const Vector me = getOrigin();
				char list[512] = {};
				int n = 0;
				for (int i = gpGlobals->maxClients + 1; i < gpGlobals->maxEntities && n < 12; ++i)
				{
					edict_t* e = engine->PEntityOfEntIndex(i);
					if (e == nullptr || e->IsFree() || e->GetClassName() == nullptr ||
						(CBotGlobals::entityOrigin(e) - me).Length() > 700.0f)
						continue;
					const size_t used = std::strlen(list);
					snprintf(list + used, sizeof(list) - used, " %d:%s:t%d", i, e->GetClassName(),
						EmpiresVehicleReadInt(this, e, "m_iTeamNum", -9));
					++n;
				}
				RCBOT_DIAG("[RCBOT2] vehicle wait userid=%d me=(%.0f %.0f %.0f) alive=%d invehicle=%d near=%s\n", userId,
					me.x, me.y, me.z, isAlive() ? 1 : 0, inVehicle, list);
			}
			return; // stage timeout counts the failure
		}
		v.vehicle = found;
		v.vehicleIndex = engine->IndexOfEdict(found);
		v.failures = 0;
		const Vector at = CBotGlobals::entityOrigin(found);
		LogEmpiresVehicleEcon("after", userId, getTeam(), m_pEdict);
		v.loadoutVerify = true; // log the bought loadout once (3.10f)
		RCBOT_DIAG("[RCBOT2] vehicle bought userid=%d vehicle=%d chassis=%d pos=(%.0f %.0f %.0f) dist=%.0f\n",
			userId, v.vehicleIndex, EmpiresVehicleReadInt(this, found, "m_iChassis", -1), at.x, at.y, at.z, bestDistance);
		setStage(EmpiresVehicleStage::APPROACH, 40.0f);
		return;
	}

	// ------------------------------------------------------- enter vehicle
	if (v.stage == EmpiresVehicleStage::APPROACH || v.stage == EmpiresVehicleStage::ENTERING)
	{
		if (vehicle == nullptr || vehicle->IsFree() || !EmpiresVehicleAlive(this, vehicle))
		{
			resetEmpiresVehicle("vehicle-gone", 5.0f);
			return;
		}
		if (inVehicle == v.vehicleIndex)
		{
			int driver = -1;
			readEmpiresEntityHandleProperty(vehicle, "m_hPassengers", 0, driver);
			if (driver != engine->IndexOfEdict(m_pEdict))
			{
				// Not the driver seat: leave again and give up on this one. The reset used to wipe the
				// IN_USE button, so the bot stayed seated and planned on foot from the seat origin
				// (TODO 3.4b, run 132330); GIVE_UP_EXIT presses USE until the bot is out.
				RCBOT_DIAG("[RCBOT2] vehicle not-driver-seat userid=%d vehicle=%d\n", userId, v.vehicleIndex);
				v.nextAction = now;
				setStage(EmpiresVehicleStage::GIVE_UP_EXIT, 3.0f);
				return;
			}
			stopMoving();
			v.lastVehicleOrigin = CBotGlobals::entityOrigin(vehicle);
			v.lastSampleTime = now;
			v.slowSince = 0.0f;
			v.ramming = false;
			setStage(EmpiresVehicleStage::DRIVING, 0.0f);
			return;
		}
		int driver = -1;
		if (readEmpiresEntityHandleProperty(vehicle, "m_hPassengers", 0, driver) && driver >= 0)
		{
			resetEmpiresVehicle("driver-seat-taken", 5.0f);
			return;
		}
		const Vector center = EntityCenter(vehicle);
		const Vector nearest = NearestOnVehicle(vehicle, getEyePosition());
		const float distance = Dist2D(getOrigin(), center);
		const float hullDist = (nearest - getEyePosition()).Length2D();
		if (v.stage == EmpiresVehicleStage::APPROACH)
		{
			if (distance > kEnterDistance && hullDist > 95.0f)
			{
				if (distance > 400.0f && RCBotNavMesh::IsLoaded())
				{
					if (m_pSchedules == nullptr || m_pSchedules->isEmpty())
					{
						if (now >= v.nextAction)
						{
							v.nextAction = now + 2.0f;
							clearNavigationAttemptCooldownForStartAnchorRetry();
							requestNavigation(nearest, RCBotNavigation::NavigationPolicy::NAVMESH_ONLY, 0);
						}
					}
				}
				else
					setMoveTo(nearest);
				return;
			}
			stopMoving();
			v.useTries = 0;
			v.nextAction = now;
			setStage(EmpiresVehicleStage::ENTERING, 8.0f);
		}
		// ENTERING: look at the vehicle body (not the bloated OBB corner) and tap use.
		VectorAngles(center - getEyePosition(), v.view);
		v.view.z = 0.0f;
		if (distance > kEnterDistance + 120.0f && hullDist > 160.0f)
		{
			setStage(EmpiresVehicleStage::APPROACH, 30.0f);
			return;
		}
		if (distance > 110.0f || hullDist > 16.0f)
			v.forward = kMaxMove * 0.5f; // creep closer while trying
		if (now >= v.nextAction)
		{
			v.buttons = IN_USE;
			v.nextAction = now + 0.6f;
			++v.useTries;
		}
		return;
	}

	// ------------------------------------------- give up a stuck vehicle
	if (v.stage == EmpiresVehicleStage::GIVE_UP_EXIT)
	{
		if (inVehicle < 0)
		{
			const Vector at = getOrigin();
			RCBOT_DIAG("[RCBOT2] vehicle bail-exit userid=%d t=0 pos=(%.0f %.0f %.0f) ground=%d\n", userId, at.x, at.y, at.z,
				GroundAt(at, at.z) ? 1 : 0);
			const bool passenger = v.passengerExit;
			resetEmpiresVehicle(passenger ? "passenger-exit" : "gave-up-stuck", passenger ? 2.0f : 30.0f);
			v.bailLogAt = now + 0.3f; // after the reset, which clears the whole brain
			v.bailLogStep = 1;
			return;
		}
		if (inVehicle != v.vehicleIndex)
		{
			resetEmpiresVehicle("vehicle-changed", 5.0f);
			return;
		}
		// The game only lets a player out where an exit point of the seat is free
		// (CEmpTank::GetExitPoint 0x69c620); a vehicle wedged against a wall or another
		// vehicle refuses USE forever. Like a human: switch to another seat (other side,
		// emp_change_seat -> CEmpPlayer::ChangeSeats 0x65ee90), take the free driver seat
		// and move a little, and finally give up and stay seated for now.
		if (v.exitSince <= 0.0f)
			v.exitSince = now;
		edict_t* exitVehicle = engine->PEntityOfEntIndex(inVehicle);
		if (now < v.exitNudgeUntil)
		{
			// Back up first (front usually hit something), then forward.
			v.forward = now < v.exitNudgeUntil - 0.8f ? -kMaxMove * 0.6f : kMaxMove * 0.6f;
			v.buttons = 0;
			return;
		}
		v.forward = 0.0f;
		constexpr float kExitStepFor = 3.0f;
		if (exitVehicle != nullptr && now - v.exitSince > kExitStepFor * (v.exitStep + 1))
		{
			const int self = engine->IndexOfEdict(m_pEdict);
			int mySeat = -1;
			int freeSeat = -1;
			bool driverFree = false;
			// m_hPassengers always has 8 entries; only the chassis' seats exist
			// (vehicle_chassis.txt, reverse-engineering/notes/vehicle-driving.md).
			static const int kChassisSeats[14] = {4, 8, 2, 2, 1, 2, 0, 0, 2, 8, 4, 2, 1, 2};
			const int chassis = EmpiresVehicleReadInt(this, exitVehicle, "m_iChassis", -1);
			const int seats = chassis >= 0 && chassis < 14 && kChassisSeats[chassis] > 0 ? kChassisSeats[chassis] : 8;
			for (int seatIndex = 0; seatIndex < seats; ++seatIndex)
			{
				int occupant = -1;
				if (!readEmpiresEntityHandleProperty(exitVehicle, "m_hPassengers", seatIndex, occupant))
					break;
				if (occupant == self)
					mySeat = seatIndex;
				else if (occupant < 0)
				{
					if (seatIndex == 0)
						driverFree = true;
					else if (freeSeat < 0 || (v.exitStep >= 2 && seatIndex > freeSeat))
						freeSeat = seatIndex;
				}
			}
			++v.exitStep;
			const char* tried = "none";
			// Driver: nudge, then the other seat (other exit points, exit_pos_<seat>_<n>), then
			// back to the driver seat; a passenger takes the free driver seat first.
			const bool driverTriesSeat = mySeat == 0 && v.exitStep == 2 && freeSeat > 0;
			if (v.exitStep <= 3 && mySeat == 0 && !driverTriesSeat)
			{
				// Driver: move the vehicle a little so an exit point becomes free.
				v.exitNudgeUntil = now + 1.6f;
				tried = "nudge";
			}
			else if (v.exitStep <= 3 && driverFree)
			{
				helpers->ClientCommand(m_pEdict, "emp_change_seat 0\n");
				tried = "driver-seat";
			}
			else if (v.exitStep <= 3 && freeSeat > 0 && freeSeat != mySeat)
			{
				char cmd[32];
				snprintf(cmd, sizeof(cmd), "emp_change_seat %d\n", freeSeat);
				helpers->ClientCommand(m_pEdict, cmd);
				tried = "other-seat";
			}
			const Vector at = CBotGlobals::entityOrigin(exitVehicle);
			RCBOT_DIAG("[RCBOT2] stall-escalate who=vehicle-exit userid=%d stage=%d reason=exit-blocked vehicle=%d seat=%d free_seat=%d driver_free=%d tried=%s at=(%.0f %.0f %.0f)\n",
				userId, v.exitStep, inVehicle, mySeat, freeSeat, driverFree ? 1 : 0, tried, at.x, at.y, at.z);
			if (v.exitStep > 3 || std::strcmp(tried, "none") == 0)
			{
				// Give up for now: stay seated; the passenger path retries after the cooldown.
				resetEmpiresVehicle("exit-blocked", 15.0f);
				return;
			}
		}
		v.buttons = 0x08000000; // handbrake (exit itself works up to 100 mph)
		if (now >= v.nextAction)
		{
			v.buttons |= IN_USE;
			v.nextAction = now + 0.6f;
		}
		return;
	}

	// ----------------------------------------- engineer repairs a vehicle
	if (v.stage == EmpiresVehicleStage::REPAIR_ASSIST)
	{
		edict_t* hurt = v.assistVehicle.get();
		RepairRequest& r = s_repairRequest[team];
		if (hurt != nullptr && !hurt->IsFree() && RepairRequestVehicle(team) == hurt && r.baseScan)
		{
			int side = -1;
			bool armorDamaged = false;
			if (!VehicleNeedsBaseRepair(this, hurt, 120.0f, side, armorDamaged))
			{
				r = RepairRequest();
				resetEmpiresVehicle("repair-done", 3.0f);
				return;
			}
			r.until = (std::max)(r.until, now + 5.0f);
			r.armorDamaged = armorDamaged;
			if (side >= 0)
				r.side = side;
		}
		if (hurt == nullptr || hurt->IsFree() || RepairRequestVehicle(team) != hurt ||
			(v.stageUntil > 0.0f && now > v.stageUntil) || !empiresRepairCallAllowed(hurt))
		{
			if (r.engineerUserId == userId)
				r.engineerUserId = -1;
			resetEmpiresVehicle(hurt == nullptr || RepairRequestVehicle(team) != hurt ? "repair-done" :
				"repair-timeout", 3.0f);
			return;
		}
		r.engineerUserId = userId;
		r.claimUntil = now + 10.0f;
		if (s_escortClaim[team].engineerUserId == userId && s_escortClaim[team].vehicle == engine->IndexOfEdict(hurt))
			s_escortClaim[team].until = now + 10.0f; // TODO 8.4: the escorted tank keeps holding
		const Vector eye = getEyePosition();
		ICollideable* box = hurt->GetCollideable();
		if (box == nullptr || Dist2D(getOrigin(), CBotGlobals::entityOrigin(hurt)) > 600.0f)
		{
			resetEmpiresVehicle("repair-too-far", 1.0f);
			return;
		}
		// Work on one side (hull repairs from anywhere; armor only on the side
		// the kit hits, CEmpTank::GetSide + HealArmorSide). Start at the weakest
		// side, move on when nothing improves for 4 s.
		// Local axes: models face +Y; 0 front +Y, 1 rear -Y, 2 left -X, 3 right +X.
		static const float kDirX[4] = {0.0f, 0.0f, -1.0f, 1.0f};
		static const float kDirY[4] = {1.0f, -1.0f, 0.0f, 0.0f};
		static const int kNextSide[4] = {3, 2, 0, 1}; // front -> right -> rear -> left
		float total = 0.0f;
		ReadVehicleHealth(hurt, total);
		for (int k = 0; k < 4; ++k)
			total += (std::max)(0, ReadArmorSide(hurt, k));
		if (v.assistSide < 0)
		{
			v.assistSide = r.side >= 0 ? r.side : 0;
			v.assistLastRise = now;
			v.assistLastTotal = total;
			v.assistWinAt = 0.0f;
		}
		// Progress = the kit mends: hull up, or one side up >= 3 in a second (kit ~4.7/s). All four
		// sides also regenerate by themselves (~1.5/s each); counting that as progress kept the
		// engineer at reach 73 for minutes with the hull stuck at 154 (K9 run 20261127-151142).
		if (now >= v.assistWinAt + 1.0f)
		{
			float hullNow = 0.0f;
			ReadVehicleHealth(hurt, hullNow);
			float maxRise = 0.0f;
			for (int k = 0; k < 4; ++k)
			{
				const float armorNow = static_cast<float>((std::max)(0, ReadArmorSide(hurt, k)));
				if (v.assistWinAt > 0.0f)
					maxRise = (std::max)(maxRise, armorNow - v.assistWinArmor[k]);
				v.assistWinArmor[k] = armorNow;
			}
			if (v.assistWinAt > 0.0f && (hullNow > v.assistWinHull + 0.5f || maxRise >= 3.0f))
			{
				v.assistLastRise = now;
				// A side takes ~27 s (126 armor at ~4.7/s); 60 s cut the job after
				// two sides (run 120808). Keep going while it mends.
				v.stageUntil = (std::max)(v.stageUntil, now + 20.0f);
			}
			v.assistWinHull = hullNow;
			v.assistWinAt = now;
		}
		v.assistLastTotal = total;
		matrix3x4_t toWorld;
		AngleMatrix(box->GetCollisionAngles(), box->GetCollisionOrigin(), toWorld);
		const Vector mins = box->OBBMins(), maxs = box->OBBMaxs();
		const Vector mid = (mins + maxs) * 0.5f;
		const float hx = (maxs.x - mins.x) * 0.5f, hy = (maxs.y - mins.y) * 0.5f;
		Vector me;
		VectorITransform(getOrigin(), toWorld, me);
		const float rx = (me.x - mid.x) / (std::max)(1.0f, hx), ry = (me.y - mid.y) / (std::max)(1.0f, hy);
		const int mySide = std::fabs(rx) > std::fabs(ry) ? (rx < 0.0f ? 2 : 3) : (ry > 0.0f ? 0 : 1);
		const int side = v.assistSide;
		auto corner = [&](int a, int b) {
			return Vector(mid.x + (kDirX[a] + kDirX[b]) * (hx + 60.0f), mid.y + (kDirY[a] + kDirY[b]) * (hy + 60.0f),
				mins.z);
		};
		Vector goalLocal;
		bool atSide = false;
		if (mySide == side)
		{
			goalLocal = Vector(mid.x + kDirX[side] * (hx + 45.0f), mid.y + kDirY[side] * (hy + 45.0f), mins.z);
			atSide = true;
		}
		else if (kDirX[mySide] + kDirX[side] == 0.0f && kDirY[mySide] + kDirY[side] == 0.0f)
		{
			// Opposite side: walk round via a corner on our side.
			const int via = kDirY[side] != 0.0f ? (rx < 0.0f ? 2 : 3) : (ry > 0.0f ? 0 : 1);
			goalLocal = corner(mySide, via);
		}
		else
			goalLocal = corner(mySide, side);
		Vector goal;
		VectorTransform(goalLocal, toWorld, goal);
		// At a corner mySide still names the old side (the shorter half-axis
		// wins the normalized test), so the corner stayed the goal and the bot
		// stood there (run diag: arrived within 4 units, my_side unchanged).
		// From the corner go on to the next leg.
		if (!atSide && Dist2D(getOrigin(), goal) < 40.0f)
		{
			if (kDirX[mySide] + kDirX[side] == 0.0f && kDirY[mySide] + kDirY[side] == 0.0f)
			{
				const int via = kDirY[side] != 0.0f ? (rx < 0.0f ? 2 : 3) : (ry > 0.0f ? 0 : 1);
				goalLocal = corner(via, side);
			}
			else
				goalLocal = Vector(mid.x + kDirX[side] * (hx + 45.0f), mid.y + kDirY[side] * (hy + 45.0f), mins.z);
			VectorTransform(goalLocal, toWorld, goal);
		}
		const Vector faceLocal(mid.x + kDirX[side] * hx, mid.y + kDirY[side] * hy, mid.z);
		Vector face;
		VectorTransform(faceLocal, toWorld, face);
		const float reach = (NearestOnVehicle(hurt, eye) - eye).Length();
		const char* kit = team == 2 ? "weapon_emp_nf_engineer_kit" : "weapon_emp_imp_engineer_kit";
		const edict_t* weapon = CClassInterface::getCurrentWeapon(m_pEdict);
		const bool kitActive = weapon != nullptr && !weapon->IsFree() && weapon->GetClassName() != nullptr &&
			std::strcmp(weapon->GetClassName(), kit) == 0;
		if (!kitActive && now >= v.nextAction)
		{
			selectWeaponName(kit);
			v.nextAction = now + 1.0f;
		}
		// Without readable side armor there is nothing to walk round for.
		// Armor only matters when some side is below full.
		const bool armorKnown = r.armorDamaged;
		// Armor work needs the middle of the side (from a corner the kit hits the
		// neighbour side, run 223745); hull work only needs reach.
		const bool standing = armorKnown ? atSide && Dist2D(getOrigin(), goal) < 60.0f && reach < 95.0f :
			reach < 75.0f;
		if (!standing)
		{
			// Pressed against the hull (reach 15, run 223126): step out first,
			// walking along it toward a corner just rubs.
			Vector walkTo = goal;
			if (reach < 45.0f)
			{
				Vector out = eye - NearestOnVehicle(hurt, eye);
				out.z = 0.0f;
				if (out.Length() < 1.0f)
					out = getOrigin() - CBotGlobals::entityOrigin(hurt);
				out.z = 0.0f;
				if (out.Length() > 1.0f)
					walkTo = getOrigin() + out.Normalized() * 100.0f;
			}
			VectorAngles(walkTo - eye, v.view);
			v.view.x = 0.0f;
			// The walk-up navigation still owns movement and makes setMoveTo
			// a no-op (m_navigationMovementOwned; runs 224451-225918).
			if (m_bEmpiresNavGoalActive)
				endEmpiresNavmeshGoal("drop", "vehicle-repair-assist", 0.0f);
			cancelNavigationAttempt("vehicle-repair-assist");
			if (m_pSchedules != nullptr && !m_pSchedules->isEmpty())
				m_pSchedules->freeMemory();
			setMoveTo(walkTo);
			v.assistOverride = false;
			// The hull mends from any side: work while passing close by.
			if (kitActive && reach < 85.0f && !armorKnown)
			{
				VectorAngles(NearestOnVehicle(hurt, eye) - eye, v.view);
				v.buttons = IN_ATTACK;
				v.assistOverride = true;
			}
			// No headway to the side (blocked corner, run 221432): try the next one.
			if (Dist2D(getOrigin(), v.assistWalkFrom) > 40.0f)
			{
				v.assistWalkFrom = getOrigin();
				v.assistWalkSince = now;
			}
			else if (now - v.assistWalkSince > 5.0f)
			{
				v.assistSide = kNextSide[side];
				v.assistWalkSince = now;
				RCBOT_DIAG("[RCBOT2] vehicle repair-side userid=%d veh=%d side=%d->%d reason=blocked\n", userId,
					engine->IndexOfEdict(hurt), side, v.assistSide);
			}
			if (total <= v.assistLastTotal + 0.5f)
				v.assistLastRise = (std::max)(v.assistLastRise, now - 2.0f); // walking is not "no progress"
		}
		else
		{
			v.assistOverride = true;
			// Nearest hull point, not the face centre: that is often beyond the
			// kit's 100 units (run 225223: right side stayed 35; aiming at the
			// nearest point mended it 35 -> 140 in run 223126).
			VectorAngles(NearestOnVehicle(hurt, eye) - eye, v.view);
			if (kitActive)
				v.buttons = IN_ATTACK;
			// The OBB overhangs the hull (gun barrel at the front, left side):
			// at reach 61-87 the 100-unit kit trace missed and front/left never
			// mended, at reach 40-44 rear/right did (run 120808). No gain for
			// 1.5 s → close in on the hull while welding.
			if (now - v.assistLastRise > 1.5f && reach > 30.0f)
				v.forward = 150.0f;
			if (now - v.assistLastRise > 4.0f)
			{
				v.assistSide = kNextSide[side];
				v.assistLastRise = now;
				RCBOT_DIAG("[RCBOT2] vehicle repair-side userid=%d veh=%d side=%d->%d total=%.0f\n", userId,
					engine->IndexOfEdict(hurt), side, v.assistSide, total);
			}
		}
		v.view.z = 0.0f;
		if (now >= v.nextLog)
		{
			v.nextLog = now + 2.0f;
			float hp = -1.0f;
			ReadVehicleHealth(hurt, hp);
			RCBOT_DIAG("[RCBOT2] vehicle repair-assist userid=%d veh=%d hp=%.0f armor=%d/%d/%d/%d side=%d my_side=%d "
				"standing=%d reach=%.0f kit=%d\n", userId, engine->IndexOfEdict(hurt), hp,
				ReadArmorSide(hurt, 0), ReadArmorSide(hurt, 1), ReadArmorSide(hurt, 2), ReadArmorSide(hurt, 3),
				side, mySide, standing ? 1 : 0, reach, kitActive ? 1 : 0);
		}
		return;
	}

	// ------------------------------------------------- Bill Nye stun attack
	if (v.stage == EmpiresVehicleStage::STUN_EXIT)
	{
		edict_t* target = v.target.get();
		if (inVehicle < 0)
		{
			selectWeaponName(team == 2 ? "weapon_emp_nf_sticky_stun_grenade" :
				"weapon_emp_imp_sticky_stun_grenade");
			v.nextAction = now + 0.9f; // weapon switch
			v.thrown = false;
			v.throwPressUntil = 0.0f;
			setStage(EmpiresVehicleStage::STUN_THROW, 6.0f);
			return;
		}
		if (target != nullptr)
		{
			VectorAngles(EntityCenter(target) - getEyePosition(), v.view);
			v.view.z = 0.0f;
		}
		v.buttons = IN_BACK; // brake; exit needs a slow vehicle
		if (now >= v.nextAction)
		{
			v.buttons |= IN_USE;
			v.nextAction = now + 0.6f;
		}
		return;
	}

	if (v.stage == EmpiresVehicleStage::STUN_THROW)
	{
		edict_t* target = v.target.get();
		if (target == nullptr || target->IsFree())
		{
			v.stunCooldownUntil = now + 20.0f;
			setStage(EmpiresVehicleStage::APPROACH, 30.0f);
			return;
		}
		const Vector aim = EntityCenter(target);
		VectorAngles(aim - getEyePosition(), v.view);
		v.view.x -= (std::min)(25.0f, distanceFrom(aim) / 40.0f); // small lob
		v.view.z = 0.0f;
		if (now < v.nextAction)
			return;
		if (!v.thrown)
		{
			if (v.throwPressUntil == 0.0f)
				v.throwPressUntil = now + 0.5f;
			if (now < v.throwPressUntil)
			{
				v.buttons = IN_ATTACK;
				return;
			}
			v.thrown = true;
			v.nextAction = now + 1.0f;
			RCBOT_DIAG("[RCBOT2] billnye stun-thrown userid=%d target=%d dist=%.0f\n", userId,
				engine->IndexOfEdict(target), distanceFrom(aim));
			return;
		}
		v.stunCooldownUntil = now + 25.0f;
		v.contactSince = 0.0f;
		v.ramming = false;
		setStage(EmpiresVehicleStage::APPROACH, 30.0f);
		return;
	}

	// ------------------------------------------------------------- driving
	if (v.stage != EmpiresVehicleStage::DRIVING)
		return;
	if (vehicle == nullptr || vehicle->IsFree() || inVehicle != v.vehicleIndex)
	{
		resetEmpiresVehicle(inVehicle < 0 ? "left-vehicle" : "vehicle-changed", 5.0f);
		return;
	}
	if (client > 0 && EmpiresLawlietShouldDisembark(team, client, v.vehicleIndex))
	{
		RCBOT_DIAG("[RCBOT2] lawliet driver-disembark userid=%d veh=%d\n", userId, v.vehicleIndex);
		v.passengerExit = true;
		v.nextAction = now;
		setStage(EmpiresVehicleStage::GIVE_UP_EXIT, 3.0f);
		return;
	}
	// Test only (TODO K9): the bot marked as human sits still like a parked player; no throttle,
	// no retreat, no ammo or repair call of its own (a human's vehicle is found by the buddy scan).
	if (rcbot_empires_test_human_hold.GetBool() && EmpiresIsHuman(m_pEdict) && !v.commanderCV)
	{
		if (!v.testHumanHoldLogged)
		{
			v.testHumanHoldLogged = true;
			RCBOT_DIAG("[RCBOT2] vehicle test-human-hold userid=%d veh=%d\n", userId, v.vehicleIndex);
		}
		return;
	}

	// A foot schedule that is still queued would plan from the seat-local origin
	// (run 215735: CFindPathTask, start (0 -117 86), start-area-not-found).
	if (!v.commanderCV && m_pSchedules != nullptr && !m_pSchedules->isEmpty())
	{
		RCBOT_DIAG("[RCBOT2] vehicle drop-foot-schedule userid=%d veh=%d\n", userId, v.vehicleIndex);
		if (m_bEmpiresNavGoalActive)
			endEmpiresNavmeshGoal("drop", "vehicle-driving", 0.0f);
		cancelNavigationAttempt("vehicle-driving");
		m_pSchedules->freeMemory();
	}

	RefreshPushZones();
	const Vector origin = CBotGlobals::entityOrigin(vehicle);
	QAngle bodyAngles(0, 0, 0);
	if (vehicle->GetCollideable() != nullptr)
		bodyAngles = vehicle->GetCollideable()->GetCollisionAngles();
	const float heading = NormalizeYaw(bodyAngles.y + 90.0f); // models face local +Y
	const int turnTestMode = rcbot_empires_vehicle_turn_test.GetInt();
	const bool turnTest = v.buyInPlace && turnTestMode >= 1 && turnTestMode <= 4 &&
		v.turnTestStart >= 0.0f;
	const float dt = now - v.lastSampleTime;
	if (dt >= (turnTest ? 0.03f : 0.1f))
	{
		v.velocity = (origin - v.lastVehicleOrigin) / dt;
		v.lastVehicleOrigin = origin;
		v.lastSampleTime = now;
	}
	const float speed = std::sqrt(v.velocity.x * v.velocity.x + v.velocity.y * v.velocity.y);
	// Signed: negative while rolling backwards (speed alone hid that, and
	// "brake" reverse throttle then drove off the rear edge, run 201712).
	const float forwardSpeed = DotProduct(v.velocity, YawDir(heading));
	const int chassis = EmpiresVehicleReadInt(this, vehicle, "m_iChassis", -1);
	if (v.width <= 0.0f)
	{
		v.width = (chassis == 0 || chassis == 8) ? 110.0f : 150.0f;
		if (vehicle->GetCollideable() != nullptr)
		{
			const Vector size = vehicle->GetCollideable()->OBBMaxs() - vehicle->GetCollideable()->OBBMins();
			if (size.x > 40.0f && size.x < 400.0f)
				v.width = size.x;
		}
		// Water: the game damages a vehicle (20 HP/s) and stops its engine only when its body centre
		// (WorldSpaceCenter) is in water; wet wheels do nothing (CEmpTank::CheckWater 0x692040,
		// reverse-engineering/notes/vehicles.md). Fords up to centre height over ground - 16.
		// Trace against buildings too (CVehicleSightFilter, not WorldOnly): inside a VF bay
		// WorldOnly traced through the factory floor to terrain 176+ HU below, clamping BE AFVs
		// to water_depth=160 so they drove straight into Palmbay sea (9.6, run 20261125-150814).
		v.waterDepth = 32.0f;
		if (vehicle->GetCollideable() != nullptr && enginetrace != nullptr)
		{
			ICollideable* box = vehicle->GetCollideable();
			const Vector centre = box->GetCollisionOrigin() +
				Vector(0, 0, 0.5f * (box->OBBMins().z + box->OBBMaxs().z));
			const float obbHeight = box->OBBMaxs().z - box->OBBMins().z;
			const float maxHeight = obbHeight > 10.0f ? 0.5f * obbHeight + 20.0f : 160.0f;
			Ray_t down;
			down.Init(centre, centre - Vector(0, 0, 400.0f));
			CVehicleSightFilter groundFilter(vehicle);
			trace_t ground;
			enginetrace->TraceRay(down, MASK_SOLID, &groundFilter, &ground);
			if (ground.fraction < 1.0f && !ground.startsolid)
			{
				const float height = (std::min)(maxHeight, centre.z - ground.endpos.z);
				v.waterDepth = (std::max)(0.0f, (std::min)(160.0f, height - 16.0f));
			}
		}
		RCBOT_DIAG("[RCBOT2] vehicle width userid=%d veh=%d chassis=%d width=%.0f water_depth=%.0f\n", userId,
			v.vehicleIndex, chassis, v.width, v.waterDepth);
	}
	const DriveWaterDepthScope waterScope(rcbot_empires_vehicle_deep_water.GetBool() ? (std::max)(1.0f, v.waterDepth) : 0.0f);
	const bool armed = chassis != 0 && chassis != 8 && !v.commanderCV;
	if (g_obstacleTest.armed || g_obstacleTest.active)
		ObstacleTestTick(v, userId, vehicle, chassis, origin, speed, forwardSpeed, now);

	// -------- cannon pivot (3.21a): every turret weapon converges on the cannon axis
	// (CEmpTank::GetWeaponVectors 0x69add0). Resolved once per tick and used for target sight,
	// the fight movement's fire line and the aim/fire line (NF heavy/medium/APC pivots sit
	// 130-151 above the hull origin; hull origin + 90 is the fallback).
	Vector cannonEye = origin + Vector(0, 0, 90);
	edict_t* cannonEdict = nullptr;
	if (armed && rcbot_empires_vehicle_cannon_eye.GetBool())
	{
		float cannonDz = 0.0f, cannonD2 = 0.0f;
		int turretIndex = -1, cannonIndex = -1;
		edict_t* turret = readEmpiresEntityHandleProperty(vehicle, "m_hTurret", 0, turretIndex) &&
			turretIndex > 0 && turretIndex < gpGlobals->maxEntities ? engine->PEntityOfEntIndex(turretIndex) : nullptr;
		edict_t* cannon = turret != nullptr && readEmpiresEntityHandleProperty(turret, "m_hCannon", 0, cannonIndex) &&
			cannonIndex > 0 && cannonIndex < gpGlobals->maxEntities ? engine->PEntityOfEntIndex(cannonIndex) : nullptr;
		if (cannon == nullptr)
		{
			// No readable handle: the emp_tank_cannon nearest to the hull (cached per vehicle).
			edict_t* cached = v.cannonCacheVehicle == v.vehicleIndex && v.cannonCacheIndex > 0 ?
				engine->PEntityOfEntIndex(v.cannonCacheIndex) : nullptr;
			if (cached != nullptr && !cached->IsFree() && IsClass(cached, "emp_tank_cannon") &&
				(CBotGlobals::entityOrigin(cached) - origin).Length() < 250.0f)
				cannon = cached;
			else if (now >= v.cannonSearchAt)
			{
				v.cannonSearchAt = now + 2.0f;
				float best = 250.0f;
				for (int i = gpGlobals->maxClients + 1; i < gpGlobals->maxEntities; ++i)
				{
					edict_t* e = engine->PEntityOfEntIndex(i);
					if (e == nullptr || e->IsFree() || !IsClass(e, "emp_tank_cannon"))
						continue;
					const float d = (CBotGlobals::entityOrigin(e) - origin).Length();
					if (d < best)
					{
						best = d;
						cannon = e;
					}
				}
				v.cannonCacheVehicle = v.vehicleIndex;
				v.cannonCacheIndex = cannon != nullptr ? engine->IndexOfEdict(cannon) : -1;
			}
			cannonIndex = cannon != nullptr ? engine->IndexOfEdict(cannon) : -1;
		}
		if (cannon != nullptr && !cannon->IsFree())
		{
			const Vector pivot = CBotGlobals::entityOrigin(cannon);
			cannonDz = pivot.z - origin.z;
			cannonD2 = Dist2D(pivot, origin);
			if ((pivot - origin).Length() < 250.0f)
			{
				cannonEye = pivot;
				cannonEdict = cannon;
			}
		}
		if (v.cannonEyeLogged != v.vehicleIndex)
		{
			v.cannonEyeLogged = v.vehicleIndex;
			RCBOT_DIAG("[RCBOT2] vehicle cannon-eye userid=%d veh=%d chassis=%d cannon_found=%d cannon=%d used=%d dz=%.1f d2=%.1f\n",
				userId, v.vehicleIndex, chassis, cannon != nullptr ? 1 : 0, cannonIndex, cannonEdict != nullptr ? 1 : 0,
				cannonDz, cannonD2);
		}
	}

	// -------- target choice
	if (now >= v.nextTargetPick)
	{
		v.nextTargetPick = now + 0.5f;
		edict_t* best = nullptr;
		float bestScore = 1.0e9f;
		// Only what a driver could see (rainbowroad 11 vs 11: tanks shot for minutes at an
		// empty jeep and through cover, user: "shoot at enemies that are not there").
		// The current target stays while seen within the last 3 s.
		const Vector eye = cannonEye;
		auto inSight = [&](edict_t* e) {
			if (e != nullptr && engine->IndexOfEdict(e) == v.fireBlockedSkipTarget && now < v.fireBlockedSkipUntil)
				return false;
			if (e == v.target.get() && now - v.fightSight < 3.0f)
				return true;
			trace_t hit;
			Ray_t ray;
			ray.Init(eye, EntityCenter(e));
			CTraceFilterWorldOnly worldOnly; // also static props (rocks, crates)
			enginetrace->TraceRay(ray, MASK_SHOT, &worldOnly, &hit);
			if (hit.fraction <= 0.97f)
				return false;
			CVehicleSightFilter filter;
			enginetrace->TraceRay(ray, MASK_SHOT, &filter, &hit);
			return hit.fraction > 0.97f ||
				(hit.m_pEnt != nullptr && e->GetUnknown() != nullptr && hit.m_pEnt == e->GetUnknown()->GetBaseEntity());
		};
		edict_t* bestEmpty = nullptr;
		float bestEmptyScore = 1500.0f; // empty enemy vehicles only close by, after everything else
		// Enemy vehicles: Bill Nye pushes them, armed vehicles shoot them.
		// Anti-APC priority #1 (5.27): enemy APCs (NF chassis=1, BE chassis=9) are mobile spawn points
		// and get top priority even when parked/empty, unless own CV is immediately threatened.
		bool bestIsApc = false;
		int bestApcChassis = -1;
		float bestApcDist = 0.0f;
		float bestApcAssetDist = 1.0e9f;
		if (v.billNye || armed)
		{
			std::vector<Vector> combatAssets;
			Vector combatOwnCv(0, 0, 0);
			bool combatOwnCvKnown = false;
			const char* ownCvCls = team == 2 ? "emp_nf_commander" : "emp_imp_commander";
			for (int a = gpGlobals->maxClients + 1; a < gpGlobals->maxEntities; ++a)
			{
				edict_t* ae = engine->PEntityOfEntIndex(a);
				if (ae == nullptr || ae->IsFree() || ae->GetClassName() == nullptr)
					continue;
				const char* aname = ae->GetClassName();
				if (!combatOwnCvKnown &&
					(std::strcmp(aname, ownCvCls) == 0 ||
					(IsClass(ae, "emp_vehicle") && EmpiresVehicleReadInt(this, ae, "m_iTeamNum", -1) == team &&
						IsCommanderChassis(EmpiresVehicleReadInt(this, ae, "m_iChassis", -1)))))
				{
					combatOwnCv = CBotGlobals::entityOrigin(ae);
					combatOwnCvKnown = true;
					combatAssets.push_back(combatOwnCv);
				}
				else if (std::strncmp(aname, "emp_building_", 13) == 0 &&
					(std::strstr(aname, "refinery") != nullptr || std::strstr(aname, "barracks") != nullptr ||
					 std::strstr(aname, "vehiclefactory") != nullptr))
				{
					int bTeam = EmpiresVehicleReadInt(this, ae, "m_iTeamNum", -1);
					if (bTeam != 2 && bTeam != 3)
						bTeam = std::strncmp(aname, "emp_building_nf_", 16) == 0 ? 2 :
							(std::strncmp(aname, "emp_building_imp_", 17) == 0 ? 3 : -1);
					if (bTeam == team)
						combatAssets.push_back(CBotGlobals::entityOrigin(ae));
				}
			}
			for (int i = gpGlobals->maxClients + 1; i < gpGlobals->maxEntities; ++i)
			{
				edict_t* e = engine->PEntityOfEntIndex(i);
				const bool enemyCommander = v.billNye &&
					IsClass(e, enemyTeam == 2 ? "emp_nf_commander" : "emp_imp_commander");
				if ((!enemyCommander && (!IsClass(e, "emp_vehicle") ||
					EmpiresVehicleReadInt(this, e, "m_iTeamNum", -1) != enemyTeam)) ||
					!EmpiresVehicleAlive(this, e))
					continue;
				const Vector at = CBotGlobals::entityOrigin(e);
				if (!GroundAt(at, at.z)) // already falling
					continue;
				const float dist = Dist2D(origin, at);
				const int eChassis = IsClass(e, "emp_vehicle") ? EmpiresVehicleReadInt(this, e, "m_iChassis", -1) : -1;
				const bool isApc = !v.billNye && IsApcChassis(eChassis);
				float score = dist;
				float assetDist = 1.0e9f;
				if (!v.billNye && !enemyCommander)
				{
					int driver = -1;
					const bool empty = !readEmpiresEntityHandleProperty(e, "m_hPassengers", 0, driver) || driver < 0;
					// Enemy armour first, but only within cannon range (user: tanks first when in
					// range, then infantry).
					if (dist >= 3500.0f || !inSight(e))
						continue;
					if (empty && !isApc)
					{
						if (dist < bestEmptyScore)
						{
							bestEmptyScore = dist;
							bestEmpty = e;
						}
						continue;
					}
					if (isApc)
					{
						for (const Vector& asset : combatAssets)
							assetDist = (std::min)(assetDist, Dist2D(asset, at));
						score -= 2500.0f;
						if (assetDist < 3000.0f)
							score -= 1000.0f;
					}
					else if (combatOwnCvKnown && Dist2D(combatOwnCv, at) < 1500.0f)
					{
						score -= 6000.0f; // immediate CV threat overrides APC priority (TODO 3.24)
					}
				}
				if (dist < (v.billNye ? 20000.0f : 4000.0f) && score < bestScore)
				{
					bestScore = score;
					best = e;
					bestIsApc = isApc;
					bestApcChassis = eChassis;
					bestApcDist = dist;
					bestApcAssetDist = assetDist;
				}
			}
		}
		// Enemy infantry on the ground: run them over or shoot them. Only with MG ammo left: the
		// cannon is not used on infantry (user 15.11.).
		if (best == nullptr && (!v.weaponsKnown || v.weaponMgLeft || v.billNye))
		{
			for (int i = 1; i <= gpGlobals->maxClients; ++i)
			{
				edict_t* e = engine->PEntityOfEntIndex(i);
				IPlayerInfo* info = e != nullptr && !e->IsFree() && playerinfomanager != nullptr ?
					playerinfomanager->GetPlayerInfo(e) : nullptr;
				if (info == nullptr || info->IsDead() || info->IsObserver() || info->GetHealth() <= 0 ||
					info->GetTeamIndex() != enemyTeam || EmpiresVehiclePlayerVehicle(this, e) >= 0)
					continue;
				int seatedCommander = 0; // a seated commander's position is the camera above the map
				if (readEmpiresEntityIntProperty(e, "m_bCommander", seatedCommander) && seatedCommander != 0)
					continue;
				const Vector at = CBotGlobals::entityOrigin(e);
				if (std::fabs(at.z - origin.z) > 160.0f)
					continue;
				const float score = Dist2D(origin, at);
				// MG on infantry up to 1200 (human test 15.11.: farther out it only sprayed empty
				// ground; that was with a wrong lead, the MG is hitscan, see 3.21 below).
				if (score < kVehicleMgInfantryRange && score < bestScore && inSight(e))
				{
					bestScore = score;
					best = e;
				}
			}
		}
		if (best == nullptr && bestEmpty != nullptr)
		{
			best = bestEmpty;
			bestScore = bestEmptyScore;
		}
		// Armed vehicles shell the enemy command vehicle and base buildings.
		if (best == nullptr && armed)
		{
			const char* cv = enemyTeam == 2 ? "emp_nf_commander" : "emp_imp_commander";
			for (int i = gpGlobals->maxClients + 1; i < gpGlobals->maxEntities; ++i)
			{
				edict_t* e = engine->PEntityOfEntIndex(i);
				if (e == nullptr || e->IsFree() || e->GetClassName() == nullptr)
					continue;
				const char* name = e->GetClassName();
				const bool isCv = std::strcmp(name, cv) == 0;
				// Commander-placed buildings use team-neutral classnames (emp_building_barracks, ...);
				// match all emp_building_* via m_iTeamNum with prefix fallback for map-placed ones.
				bool isBuilding = std::strncmp(name, "emp_building_", 13) == 0 &&
					std::strstr(name, "physbox") == nullptr && std::strstr(name, "panel") == nullptr;
				if (isBuilding)
				{
					int buildingTeam = EmpiresVehicleReadInt(this, e, "m_iTeamNum", -1);
					if (buildingTeam != 2 && buildingTeam != 3)
						buildingTeam = std::strncmp(name, "emp_building_nf_", 16) == 0 ? 2 :
							(std::strncmp(name, "emp_building_imp_", 17) == 0 ? 3 : -1);
					isBuilding = buildingTeam == enemyTeam;
				}
				if (!isCv && !isBuilding)
					continue;
				if (isBuilding && EmpiresVehicleReadInt(this, e, "m_iHealth", 1) <= 0)
					continue;
				const float score = Dist2D(origin, CBotGlobals::entityOrigin(e)) + (isCv ? 0.0f : 500.0f);
				// Last resort only (vehicles and infantry come first, user 20.11.), and only what the
				// driver can see: no shelling through hills and other buildings.
				if (score < 4000.0f && score < bestScore && inSight(e))
				{
					bestScore = score;
					best = e;
				}
			}
		}
		if (best != v.target.get())
		{
			if (best != nullptr && bestIsApc)
				RCBOT_DIAG("[RCBOT2] anti-apc-priority who=vehicle userid=%d apc=%d chassis=%d dist=%.0f asset_dist=%.0f\n",
					userId, engine->IndexOfEdict(best), bestApcChassis, bestApcDist, bestApcAssetDist);
			v.aimSince = now;
			v.contactSince = 0.0f;
			v.ramming = false;
		}
		v.target = best;

		// Strategic goal when no target is in sight (TODO 3.24):
		// 1) Defend own base/CV from enemy vehicles within 3000 HU
		// 2) Support contested/enemy front flags on flag maps
		// 3) Hit closer enemy forward buildings (refineries/barracks/VF/radar)
		// 4) Fall back to the enemy command vehicle.
		v.goalValid = false;
		int goalKind = -1;
		Vector ownCvOrigin(0, 0, 0), enemyCvOrigin(0, 0, 0);
		bool ownCvKnown = false, enemyCvKnown = false;
		const char* ownCvClass = team == 2 ? "emp_nf_commander" : "emp_imp_commander";
		const char* enemyCvClass = enemyTeam == 2 ? "emp_nf_commander" : "emp_imp_commander";
		for (int i = gpGlobals->maxClients + 1; i < gpGlobals->maxEntities; ++i)
		{
			edict_t* e = engine->PEntityOfEntIndex(i);
			if (e == nullptr || e->IsFree() || e->GetClassName() == nullptr)
				continue;
			if (!enemyCvKnown &&
				((IsClass(e, "emp_vehicle") && EmpiresVehicleReadInt(this, e, "m_iTeamNum", -1) == enemyTeam &&
					IsCommanderChassis(EmpiresVehicleReadInt(this, e, "m_iChassis", -1))) ||
				IsClass(e, enemyCvClass)))
			{
				enemyCvOrigin = CBotGlobals::entityOrigin(e);
				enemyCvKnown = true;
			}
			else if (!ownCvKnown &&
				((IsClass(e, "emp_vehicle") && EmpiresVehicleReadInt(this, e, "m_iTeamNum", -1) == team &&
					IsCommanderChassis(EmpiresVehicleReadInt(this, e, "m_iChassis", -1))) ||
				IsClass(e, ownCvClass)))
			{
				ownCvOrigin = CBotGlobals::entityOrigin(e);
				ownCvKnown = true;
			}
		}
		// 3.20 part B: with fair intel the tank drives to where the team knows the enemy CV
		// (live sight, last sighting, else the initial enemy base), not to its live position.
		const bool fairIntel = EmpiresFairIntelEnabled(team);
		if (fairIntel && enemyCvKnown)
		{
			Vector known;
			bool live = false;
			if (EmpiresTeamIntelEnemyBase(team, known, live))
				enemyCvOrigin = known;
		}
		const float enemyCvDist = enemyCvKnown ? Dist2D(origin, enemyCvOrigin) : 1.0e9f;
		if (armed && !v.billNye)
		{
			// Tank role in the game flow (user 20.11.): defend, hunt spotted armour, support the
			// front from outside, push forward economy only when the front holds, CV last.
			auto worldClear = [&](const Vector& from, const Vector& to) {
				trace_t hit;
				Ray_t ray;
				ray.Init(from, to);
				CTraceFilterWorldOnly worldOnly;
				enginetrace->TraceRay(ray, MASK_SHOT, &worldOnly, &hit);
				return hit.fraction > 0.97f;
			};
			std::vector<Vector> ownAssets;
			if (ownCvKnown)
				ownAssets.push_back(ownCvOrigin);
			for (int i = gpGlobals->maxClients + 1; i < gpGlobals->maxEntities; ++i)
			{
				edict_t* e = engine->PEntityOfEntIndex(i);
				if (e == nullptr || e->IsFree() || e->GetClassName() == nullptr ||
					std::strncmp(e->GetClassName(), "emp_building_", 13) != 0 ||
					(std::strstr(e->GetClassName(), "refinery") == nullptr &&
					 std::strstr(e->GetClassName(), "barracks") == nullptr &&
					 std::strstr(e->GetClassName(), "vehiclefactory") == nullptr))
					continue;
				int buildingTeam = EmpiresVehicleReadInt(this, e, "m_iTeamNum", -1);
				if (buildingTeam != 2 && buildingTeam != 3)
					buildingTeam = std::strncmp(e->GetClassName(), "emp_building_nf_", 16) == 0 ? 2 :
						(std::strncmp(e->GetClassName(), "emp_building_imp_", 17) == 0 ? 3 : -1);
				if (buildingTeam == team)
					ownAssets.push_back(CBotGlobals::entityOrigin(e));
			}
			// 1. Defend: enemy armour within 3000 of the own CV or an own refinery/barracks/VF.
			// 2. Hunt: enemy armour within 6000 that this tank or a living teammate sees.
			// Enemy APCs (5.27) receive top priority even when parked/unmanned.
			float bestDefend = 1.0e9f, bestHunt = 1.0e9f;
			Vector defendAt(0, 0, 0), huntAt(0, 0, 0);
			int defendApcEnt = -1, huntApcEnt = -1;
			int defendApcChassis = -1, huntApcChassis = -1;
			float defendApcDist = 0.0f, huntApcDist = 0.0f;
			float defendApcAssetDist = 1.0e9f, huntApcAssetDist = 1.0e9f;
			for (int i = gpGlobals->maxClients + 1; i < gpGlobals->maxEntities; ++i)
			{
				edict_t* e = engine->PEntityOfEntIndex(i);
				const int eChassis = IsClass(e, "emp_vehicle") ? EmpiresVehicleReadInt(this, e, "m_iChassis", -1) : -1;
				if (!IsClass(e, "emp_vehicle") || EmpiresVehicleReadInt(this, e, "m_iTeamNum", -1) != enemyTeam ||
					!EmpiresVehicleAlive(this, e) || IsCommanderChassis(eChassis))
					continue;
				const bool isApc = IsApcChassis(eChassis);
				int driver = -1;
				if (!isApc && (!readEmpiresEntityHandleProperty(e, "m_hPassengers", 0, driver) || driver < 0))
					continue; // empty wrecks and parked non-APC vehicles are not a threat
				Vector at = CBotGlobals::entityOrigin(e);
				if (fairIntel && !EmpiresTeamIntelKnownPos(team, e, at))
					continue; // 3.20 part B: armour nobody on the team has seen (20 s memory)
				const float d = Dist2D(origin, at);
				float minAssetDist = 1.0e9f;
				for (const Vector& asset : ownAssets)
					minAssetDist = (std::min)(minAssetDist, Dist2D(asset, at));
				const bool nearAsset = minAssetDist < (isApc ? 3500.0f : 3000.0f);
				const bool immediateCvThreat = !isApc && ownCvKnown && Dist2D(ownCvOrigin, at) < 1500.0f;
				const float defendScore = d - (immediateCvThreat ? 6000.0f : (isApc ? 3000.0f : 0.0f));
				const float huntScore = d - (isApc ? 3000.0f : 0.0f);
				const bool canDefend = nearAsset && defendScore < bestDefend;
				const bool canHunt = d < (isApc ? 7000.0f : 6000.0f) && huntScore < bestHunt;
				if (!canDefend && !canHunt)
					continue;
				bool spotted = fairIntel; // on the team board = spotted by a teammate, radar, camera or game spotting
				if (!fairIntel && (canHunt || isApc))
				{
					const Vector centre = EntityCenter(e);
					spotted = d < 4000.0f && worldClear(origin + Vector(0, 0, 90), centre);
					for (int p = 1; p <= gpGlobals->maxClients && !spotted; ++p)
					{
						edict_t* mate = engine->PEntityOfEntIndex(p);
						if (mate == nullptr || mate->IsFree() || mate == m_pEdict || CClassInterface::getTeam(mate) != team ||
							!CBotGlobals::entityIsAlive(mate))
							continue;
						const Vector eye = CBotGlobals::entityOrigin(mate) + Vector(0, 0, 64);
						spotted = (eye - centre).Length() < 3500.0f && worldClear(eye, centre);
					}
				}
				if (canDefend && (!isApc || spotted))
				{
					bestDefend = defendScore;
					defendAt = at;
					defendApcEnt = isApc ? i : -1;
					defendApcChassis = eChassis;
					defendApcDist = d;
					defendApcAssetDist = minAssetDist;
				}
				if (canHunt && spotted)
				{
					bestHunt = huntScore;
					huntAt = at;
					huntApcEnt = isApc ? i : -1;
					huntApcChassis = eChassis;
					huntApcDist = d;
					huntApcAssetDist = minAssetDist;
				}
			}
			const bool spawnApc = IsApcChassis(chassis) && !EmpiresLawlietIsCrew(team, client);
			// Spawn APC park/staging spot: >= 800 from own buildings and >= 400 from other own
			// vehicles. Crossroads A/B 20261126-102206 / 220248: the staging point (2149 -375) sat on
			// the middle crossing 600 from the own forward refinery; stuck APCs were abandoned there
			// and own defenders wedged themselves in them (two APCs side by side formed a pocket).
			std::vector<Vector> ownParkBlockers;
			std::vector<Vector> ownParkVehicles;
			bool ownParkScanned = false;
			auto apcSpotClearOfOwn = [&](const Vector& spot) {
				if (!ownParkScanned)
				{
					ownParkScanned = true;
					for (int i = gpGlobals->maxClients + 1; i < gpGlobals->maxEntities; ++i)
					{
						edict_t* e = engine->PEntityOfEntIndex(i);
						if (e == nullptr || e->IsFree() || e->GetClassName() == nullptr)
							continue;
						const char* name = e->GetClassName();
						if (std::strcmp(name, "emp_vehicle") == 0)
						{
							if (i != v.vehicleIndex && EmpiresVehicleReadInt(this, e, "m_iTeamNum", -1) == team &&
								EmpiresVehicleAlive(this, e))
								ownParkVehicles.push_back(CBotGlobals::entityOrigin(e));
							continue;
						}
						if (std::strncmp(name, "emp_building_", 13) != 0 || std::strstr(name, "physbox") != nullptr ||
							std::strstr(name, "panel") != nullptr)
							continue;
						int buildingTeam = EmpiresVehicleReadInt(this, e, "m_iTeamNum", -1);
						if (buildingTeam != 2 && buildingTeam != 3)
							buildingTeam = std::strncmp(name, "emp_building_nf_", 16) == 0 ? 2 :
								(std::strncmp(name, "emp_building_imp_", 17) == 0 ? 3 : -1);
						if (buildingTeam == team)
							ownParkBlockers.push_back(CBotGlobals::entityOrigin(e));
					}
				}
				for (const Vector& at : ownParkBlockers)
					if (Dist2D(at, spot) < 800.0f)
						return false;
				for (const Vector& at : ownParkVehicles)
					if (Dist2D(at, spot) < 400.0f)
						return false;
				return !WaitSpotTaken(team, v.vehicleIndex, spot, now);
			};
			int rejOwnPark = 0;
			if (!spawnApc && bestDefend < 1.0e9f)
			{
				v.goal = defendAt;
				v.goalValid = true;
				goalKind = 0;
				if (defendApcEnt > 0)
					RCBOT_DIAG("[RCBOT2] anti-apc-priority who=vehicle-goal userid=%d apc=%d chassis=%d dist=%.0f asset_dist=%.0f\n",
						userId, defendApcEnt, defendApcChassis, defendApcDist, defendApcAssetDist);
			}
			else if (!spawnApc && bestHunt < 1.0e9f)
			{
				v.goal = huntAt;
				v.goalValid = true;
				goalKind = 1;
				if (huntApcEnt > 0)
					RCBOT_DIAG("[RCBOT2] anti-apc-priority who=vehicle-goal userid=%d apc=%d chassis=%d dist=%.0f asset_dist=%.0f\n",
						userId, huntApcEnt, huntApcChassis, huntApcDist, huntApcAssetDist);
			}
			// 3. Front: a contested flag (own one under attack, or one we can take). The tank holds a
			// reachable spot about 1200 HU before it on its own side and never drives in (emp_coast:
			// centre flag in a bunker).
			const CBotMod* pMod = CBotGlobals::getCurrentMod();
			if (!v.goalValid && pMod != nullptr && pMod->getBotType() == BOTTYPE_EMPIRES)
			{
				bool found = false;
				float bestFront = 1.0e9f;
				Vector front(0, 0, 0);
				for (const CEmpiresMod::CapturePointObservation& pt :
					static_cast<const CEmpiresMod*>(pMod)->getCapturePoints())
				{
					if (!pt.originKnown)
						continue;
					const bool ownPoint = pt.teamOwnerKnown && pt.teamOwner == team;
					const bool attacked = ownPoint && (team == 2 ?
						(!pt.beOccupantUserIds.empty() || (pt.beStatusKnown && pt.beStatus == 3)) :
						(!pt.nfOccupantUserIds.empty() || (pt.nfStatusKnown && pt.nfStatus == 3)));
					const bool cappable = !ownPoint && (team == 2 ?
						(!pt.nfCappableKnown || pt.nfCappable != 0) :
						(!pt.beCappableKnown || pt.beCappable != 0));
					if (spawnApc && now < v.skipFrontUntil && Dist2D(pt.origin, v.skippedFront) < 50)
						continue;
					if (!attacked && !cappable)
						continue;
					// The front nearest to the own side first (own CV, else this tank).
					const float d = Dist2D(ownCvKnown ? ownCvOrigin : origin, pt.origin) - (attacked ? 1500.0f : 0.0f);
					if (d < bestFront)
					{
						bestFront = d;
						front = pt.origin;
						found = true;
					}
				}
				// 8.4: no contested flag (emp_crossroads has none): the spawn APC takes the enemy
				// building nearest to the own side (else the enemy CV) as its front and parks out of
				// its sight instead of pushing into it (runs 20261124-2244*: 0x front, APCs on push/cv).
				bool enemyAnchor = false;
				if (!found && spawnApc && rcbot_empires_apc_cover_park.GetBool())
				{
					const Vector side = ownCvKnown ? ownCvOrigin : origin;
					float bestAnchor = 1.0e9f;
					for (int i = gpGlobals->maxClients + 1; i < gpGlobals->maxEntities; ++i)
					{
						edict_t* e = engine->PEntityOfEntIndex(i);
						if (e == nullptr || e->IsFree() || e->GetClassName() == nullptr)
							continue;
						const char* name = e->GetClassName();
						if (std::strncmp(name, "emp_building_", 13) != 0 || std::strstr(name, "physbox") != nullptr ||
							std::strstr(name, "panel") != nullptr)
							continue;
						if (std::strstr(name, "refinery") == nullptr && std::strstr(name, "barracks") == nullptr &&
							std::strstr(name, "vehiclefactory") == nullptr && std::strstr(name, "radar") == nullptr &&
							std::strstr(name, "turret") == nullptr)
							continue;
						int buildingTeam = EmpiresVehicleReadInt(this, e, "m_iTeamNum", -1);
						if (buildingTeam != 2 && buildingTeam != 3)
							buildingTeam = std::strncmp(name, "emp_building_nf_", 16) == 0 ? 2 :
								(std::strncmp(name, "emp_building_imp_", 17) == 0 ? 3 : -1);
						if (buildingTeam != enemyTeam || EmpiresVehicleReadInt(this, e, "m_iHealth", 1) <= 0)
							continue;
						Vector at = CBotGlobals::entityOrigin(e);
						if (fairIntel && !EmpiresTeamIntelKnownPos(team, e, at))
							continue;
						if (now < v.skipFrontUntil && Dist2D(at, v.skippedFront) < 50)
							continue;
						const float d = Dist2D(side, at);
						if (d < bestAnchor)
						{
							bestAnchor = d;
							front = at;
						}
					}
					if (bestAnchor < 1.0e9f)
						found = true;
					else if (enemyCvKnown && !(now < v.skipFrontUntil && Dist2D(enemyCvOrigin, v.skippedFront) < 50))
					{
						front = enemyCvOrigin;
						found = true;
					}
					enemyAnchor = found;
				}
				if (found)
				{
					const bool changed = Dist2D(front, v.supportFlag) > 50.0f;
					// Another vehicle of the team holds this spot (it claimed it while this one hunted or
					// both picked before a claim existed): search a free one.
					if (v.supportKnown && !changed && WaitSpotTaken(team, v.vehicleIndex, v.supportSpot, now))
					{
						RCBOT_DIAG("[RCBOT2] vehicle wait-spread userid=%d reason=taken spot=(%.0f %.0f %.0f)\n", userId,
							v.supportSpot.x, v.supportSpot.y, v.supportSpot.z);
						v.supportKnown = false;
						if (spawnApc)
							v.nextParkSearch = now;
					}
					if (spawnApc && v.supportKnown && !changed)
					{
						const float left = Dist2D(origin, v.supportSpot);
						if (left < 250.0f || left + 100.0f < v.parkBestDistance || v.retreating)
						{
							v.parkBestDistance = left;
							v.parkProgressAt = now;
						}
						const bool exposed = best != nullptr && !v.retreating && worldClear(EntityCenter(best), v.supportSpot + Vector(0, 0, 64));
						if (exposed || now - v.parkProgressAt > 20.0f)
						{
							v.rejectedPark = v.supportSpot;
							v.rejectedParkUntil = now + 45.0f;
							v.supportKnown = false;
							RCBOT_DIAG("[RCBOT2] stall-escalate who=apc-park stage=1 reason=%s\n", exposed ? "exposed" : "no-progress");
						}
					}
					if ((!spawnApc && (changed || !v.supportKnown)) ||
						(spawnApc && ((changed && v.supportKnown) || now >= v.nextParkSearch)))
					{
						// A periodic re-search that finds nothing keeps the current spot (run 20261125-105533:
						// spot <-> staging every 5 s while an enemy saw the candidates); exposed/no-progress drop it above.
						const bool hadSpot = spawnApc && v.supportKnown && !changed;
						const Vector prevSpot = v.supportSpot;
						v.supportFlag = front;
						v.supportKnown = false;
						Vector home = (ownCvKnown ? ownCvOrigin : origin) - front;
						home.z = 0.0f;
						const float homeYaw = home.Length() > 1.0f ? RAD2DEG(std::atan2(home.y, home.x)) : 0.0f;
						static const float kOffsets[] = {0.0f, 30.0f, -30.0f, 60.0f, -60.0f, 90.0f, -90.0f};
						static const float kFlanks[] = {60, -60, 90, -90, 30, -30, 0};
						v.nextParkSearch = now + 5.0f;
						Vector fallback(0, 0, 0);
						bool fallbackKnown = false;
						int rejMesh = 0, rejWalk = 0, rejSeen = 0, rejFoot = 0, rejDrive = 0;
						// Every free spot claimed by other vehicles: still take the first one rather than none.
						Vector takenFallback(0, 0, 0);
						bool takenFallbackKnown = false;
						// 5.14a: the spawn APC tries the middle ring first (1200 sat in the fight at flags).
						static const float kRadii[] = {1200.0f, 1600.0f, 2000.0f};
						static const float kApcRadii[] = {1600.0f, 1200.0f, 2000.0f};
						for (float radius : (spawnApc ? kApcRadii : kRadii))
						{
							if (radius > 1900.0f && !enemyAnchor)
								break; // third ring only around an enemy building (narrow Crossroads road)
							for (float offset : (spawnApc ? kFlanks : kOffsets))
							{
								Vector mesh;
								const Vector probe = front + YawDir(homeYaw + offset) * radius;
								// Room for a vehicle, not only infantry mesh (emp_coast: BE tank stuck at the bunker
								// support spot (1650 1275), run 143630).
								if (!RCBotNavMesh::ClosestPointOnMesh(probe, 400.0f, mesh) || Dist2D(mesh, front) < 900.0f ||
									!VehicleSpotFree(mesh))
								{
									++rejMesh;
									continue;
								}
								// Boat flags on Palmbay can project the support spot onto the seabed.
								// Check the actual parking point before accepting either candidate.
								const Vector waterProbe = mesh + Vector(0, 0, (std::max)(1.0f, v.waterDepth));
								if (enginetrace != nullptr && (enginetrace->GetPointContents(waterProbe) & MASK_WATER) != 0)
									continue;
								if (spawnApc)
								{
									const float walk = Dist2D(mesh, front);
									if (walk < 1000 || walk > (enemyAnchor ? 2300.0f : 1800.0f) ||
										(now < v.rejectedParkUntil && Dist2D(mesh, v.rejectedPark) < 400) ||
										(best != nullptr && worldClear(EntityCenter(best), mesh + Vector(0, 0, 64))))
									{
										++rejWalk;
										continue;
									}
									if (!apcSpotClearOfOwn(mesh))
									{
										++rejOwnPark;
										continue;
									}
									// Behind a crest/building: the enemy building (and its turrets) must not see the hull.
									if (enemyAnchor && (worldClear(front + Vector(0, 0, 64), mesh + Vector(0, 0, 64)) ||
										worldClear(front + Vector(0, 0, 160), mesh + Vector(0, 0, 90))))
									{
										++rejSeen;
										continue;
									}
									RCBotNavMesh::Path foot, drive;
									Vector footGoal = front;
									if (enemyAnchor)
										RCBotNavMesh::ClosestPointOnMesh(front, 600.0f, footGoal);
									if (RCBotNavMesh::BuildPath(mesh, footGoal, foot) != RCBotNavMesh::PathResult::SUCCESS ||
										foot.length > 2400 || foot.points.empty() || (foot.points.back() - footGoal).Length() > 250)
									{
										++rejFoot;
										continue;
									}
									if (RCBotNavMesh::BuildVehiclePath(origin, mesh, drive, v.width, false, v.waterDepth) != RCBotNavMesh::PathResult::SUCCESS)
									{
										++rejDrive;
										continue;
									}
									v.supportSpot = mesh;
									v.supportKnown = true;
									v.parkBestDistance = Dist2D(origin, mesh);
									v.parkProgressAt = now;
									break;
								}
								if (WaitSpotTaken(team, v.vehicleIndex, mesh, now))
								{
									if (!takenFallbackKnown)
									{
										takenFallback = mesh;
										takenFallbackKnown = true;
									}
									++rejOwnPark;
									continue;
								}
								if (!fallbackKnown)
								{
									fallback = mesh;
									fallbackKnown = true;
								}
								if (worldClear(mesh + Vector(0, 0, 90), front + Vector(0, 0, 64)))
								{
									v.supportSpot = mesh;
									v.supportKnown = true;
									break;
								}
							}
							if (v.supportKnown)
								break;
						}
						if (!v.supportKnown && fallbackKnown)
						{
							v.supportSpot = fallback;
							v.supportKnown = true;
						}
						if (!v.supportKnown && takenFallbackKnown)
						{
							v.supportSpot = takenFallback;
							v.supportKnown = true;
						}
						if (!v.supportKnown && hadSpot)
						{
							v.supportSpot = prevSpot;
							v.supportKnown = true;
						}
						RCBOT_DIAG("[RCBOT2] vehicle front userid=%d flag=(%.0f %.0f %.0f) support=(%.0f %.0f %.0f) known=%d anchor=%s rej=mesh:%d,walk:%d,seen:%d,foot:%d,drive:%d,own:%d\n", userId,
							front.x, front.y, front.z, v.supportSpot.x, v.supportSpot.y, v.supportSpot.z, v.supportKnown ? 1 : 0,
							enemyAnchor ? "enemy-building" : "flag", rejMesh, rejWalk, rejSeen, rejFoot, rejDrive, rejOwnPark);
						if (enemyAnchor && v.supportKnown)
							RCBOT_DIAG("[RCBOT2] apc-cover-park userid=%d anchor=(%.0f %.0f) spot=(%.0f %.0f %.0f) walk=%.0f\n", userId,
								front.x, front.y, v.supportSpot.x, v.supportSpot.y, v.supportSpot.z, Dist2D(v.supportSpot, front));
					}
					if (spawnApc && !v.supportKnown && enemyAnchor)
					{
						// No hidden spot (yet): wait at a staging point 2500 HU before the enemy building on
						// the own side instead of pushing into it (run 20261125-104323: push/cv into the base).
						Vector home = (ownCvKnown ? ownCvOrigin : origin) - front;
						home.z = 0.0f;
						Vector stage = origin; // nothing on the mesh: hold here rather than push in
						bool staged = false;
						for (float back : {2500.0f, 3000.0f, 2000.0f, 3500.0f, 4500.0f})
							if (!staged && home.Length() > 1.0f &&
								RCBotNavMesh::ClosestPointOnMesh(front + home.Normalized() * back, 800.0f, stage) &&
								Dist2D(stage, front) > 1500.0f)
							{
								if (apcSpotClearOfOwn(stage))
									staged = true;
								else
									++rejOwnPark;
							}
						if (!staged)
							stage = Dist2D(origin, front) > 2000.0f ? origin : (ownCvKnown ? ownCvOrigin : origin);
						{
							const bool newStage = Dist2D(v.goal, stage) > 50.0f;
							v.goal = stage;
							if (staged)
								ClaimWaitSpot(team, v.vehicleIndex, stage, now);
							v.goalValid = true;
							goalKind = 2;
							if (newStage)
								RCBOT_DIAG("[RCBOT2] apc-cover-park userid=%d stage=%s anchor=(%.0f %.0f) spot=(%.0f %.0f %.0f) rej_own=%d\n",
									userId, staged ? "staging" : "hold", front.x, front.y, stage.x, stage.y, stage.z, rejOwnPark);
						}
					}
					else if (spawnApc && !v.supportKnown)
					{
						v.skippedFront = front;
						v.skipFrontUntil = now + 20.0f;
					}
					if (v.supportKnown)
					{
						v.goal = v.supportSpot;
						v.goalValid = true;
						goalKind = 2;
						ClaimWaitSpot(team, v.vehicleIndex, v.supportSpot, now);
					}
				}
			}
			// 4. Push: when the front holds, closer enemy economy/spawn buildings (refinery/barracks/VF/radar).
			// Not for a spawn APC (5.14a): it parks out of sight or waits, it does not push in like a tank.
			if (!v.goalValid && !(spawnApc && rcbot_empires_apc_cover_park.GetBool()))
			{
				float bestBldgDist = enemyCvDist - 1500.0f;
				for (int i = gpGlobals->maxClients + 1; i < gpGlobals->maxEntities; ++i)
				{
					edict_t* e = engine->PEntityOfEntIndex(i);
					if (e == nullptr || e->IsFree() || e->GetClassName() == nullptr)
						continue;
					const char* name = e->GetClassName();
					if (std::strncmp(name, "emp_building_", 13) != 0 ||
						std::strstr(name, "physbox") != nullptr ||
						std::strstr(name, "panel") != nullptr)
						continue;
					if (std::strstr(name, "refinery") == nullptr &&
						std::strstr(name, "barracks") == nullptr &&
						std::strstr(name, "vehiclefactory") == nullptr &&
						std::strstr(name, "radar") == nullptr)
						continue;
					int buildingTeam = EmpiresVehicleReadInt(this, e, "m_iTeamNum", -1);
					if (buildingTeam != 2 && buildingTeam != 3)
						buildingTeam = std::strncmp(name, "emp_building_nf_", 16) == 0 ? 2 :
							(std::strncmp(name, "emp_building_imp_", 17) == 0 ? 3 : -1);
					if (buildingTeam != enemyTeam || EmpiresVehicleReadInt(this, e, "m_iHealth", 1) <= 0)
						continue;
					Vector at = CBotGlobals::entityOrigin(e);
					if (fairIntel && !EmpiresTeamIntelKnownPos(team, e, at))
						continue;
					const float d = Dist2D(origin, at);
					if (d < bestBldgDist)
					{
						bestBldgDist = d;
						v.goal = at;
						v.goalValid = true;
						goalKind = 3;
					}
				}
				// Hold about 1000 HU before the building on the own side instead of driving into its box, fanned
				// out per bot (0 / +300 / -300) so several tanks on the same single known building stand side by
				// side (crossroads 20261126-105230: three BE tanks jammed at NF barracks (3161 -2900), 3 give-ups).
				if (goalKind == 3)
				{
					const Vector building = v.goal;
					if (!v.pushHoldKnown || Dist2D(v.pushHoldFor, building) > 50.0f || now >= v.pushHoldUntil)
					{
						v.pushHoldFor = building;
						v.pushHoldUntil = now + 10.0f;
						v.pushHoldKnown = false;
						Vector home = (ownCvKnown ? ownCvOrigin : origin) - building;
						home.z = 0.0f;
						if (home.Length2D() < 1.0f)
							home = origin - building;
						home.z = 0.0f;
						if (home.Length2D() > 1.0f)
						{
							home.NormalizeInPlace();
							const Vector side(-home.y, home.x, 0.0f);
							static const float kLaterals[] = {0.0f, 300.0f, -300.0f};
							const int first = (userId > 0 ? userId : 0) % 3;
							int tried = 0;
							for (float range : {1000.0f, 1300.0f, 800.0f})
							{
								for (int k = 0; k < 3 && !v.pushHoldKnown; ++k)
								{
									const float lateral = kLaterals[(first + k) % 3];
									Vector mesh;
									++tried;
									if (!RCBotNavMesh::ClosestPointOnMesh(building + home * range + side * lateral, 300.0f, mesh) ||
										Dist2D(mesh, building) < 700.0f || !VehicleSpotFree(mesh))
										continue;
									if (enginetrace != nullptr &&
										(enginetrace->GetPointContents(mesh + Vector(0, 0, (std::max)(1.0f, v.waterDepth))) & MASK_WATER) != 0)
										continue;
									if (!worldClear(mesh + Vector(0, 0, 90), building + Vector(0, 0, 64)))
										continue;
									v.pushHold = mesh;
									v.pushHoldKnown = true;
									RCBOT_DIAG("[RCBOT2] vehicle push-hold userid=%d building=(%.0f %.0f) hold=(%.0f %.0f %.0f) range=%.0f lateral=%.0f tried=%d\n",
										userId, building.x, building.y, mesh.x, mesh.y, mesh.z, range, lateral, tried);
								}
								if (v.pushHoldKnown)
									break;
							}
							if (!v.pushHoldKnown)
								RCBOT_DIAG("[RCBOT2] vehicle push-hold userid=%d building=(%.0f %.0f) none tried=%d\n",
									userId, building.x, building.y, tried);
						}
					}
					if (v.pushHoldKnown)
						v.goal = v.pushHold;
				}
			}
		}
		if (!v.goalValid && enemyCvKnown &&
			!(IsApcChassis(chassis) && !EmpiresLawlietIsCrew(team, client) && rcbot_empires_apc_cover_park.GetBool()))
		{
			v.goal = enemyCvOrigin;
			v.goalValid = true;
			goalKind = 4;
		}
		if (v.goalValid && (goalKind != v.goalKind || Dist2D(v.goal, v.goalLogged) > 500.0f))
		{
			static const char* const kKinds[] = {"defend", "hunt", "front", "push", "cv"};
			RCBOT_DIAG("[RCBOT2] vehicle goal userid=%d kind=%s at=(%.0f %.0f %.0f) dist=%.0f fair=%d\n", userId,
				goalKind >= 0 ? kKinds[goalKind] : "-", v.goal.x, v.goal.y, v.goal.z, Dist2D(origin, v.goal), fairIntel ? 1 : 0);
			v.goalKind = goalKind;
			v.goalLogged = v.goal;
		}
	}

	if (v.commanderCV)
	{
		// Boomtanking: only enemy infantry inside the base zone (user: do not chase across the map).
		// Own pick (same rule as the trigger in the commander economy): the general
		// pick above left the target empty in run 154412.
		// New prey only inside the zone; the current one is followed to zone + 1000 (run 154939:
		// dropped at 3034 against 3023 right after the ram started).
		edict_t* prey = nullptr;
		float preyDistance = 3000.0f;
		edict_t* current = v.target.get();
		if (current != nullptr && !current->IsFree() && !CBotGlobals::entityIsAlive(current) &&
			engine->IndexOfEdict(current) == v.cvLastTarget)
			RCBOT_DIAG("[RCBOT2] cv-hunt prey-dead userid=%d prey=%d dist=%.0f speed=%.0f\n", userId,
				engine->IndexOfEdict(current), Dist2D(origin, CBotGlobals::entityOrigin(current)), speed);
		for (int i = 1; v.cvHunt && i <= gpGlobals->maxClients; ++i)
		{
			edict_t* e = engine->PEntityOfEntIndex(i);
			if (e == nullptr || e->IsFree() || CClassInterface::getTeam(e) != enemyTeam ||
				!CBotGlobals::entityIsAlive(e) || EmpiresVehiclePlayerVehicle(this, e) >= 0)
				continue;
			const Vector at = CBotGlobals::entityOrigin(e);
			const float d = Dist2D(origin, at) - (e == current ? 500.0f : 0.0f); // stay on the current prey
			const float zone = v.cvZone + (e == current ? 1000.0f : 0.0f);
			if (Dist2D(at, v.cvHome) < zone && std::fabs(at.z - origin.z) < 160.0f && d < preyDistance)
			{
				prey = e;
				preyDistance = d;
			}
		}
		const bool keep = prey != nullptr;
		if (prey != v.target.get())
		{
			v.aimSince = now;
			v.contactSince = 0.0f;
			v.ramming = false;
		}
		v.target = prey;
		if (keep)
			v.cvNoPreySince = 0.0f;
		else if (v.cvNoPreySince == 0.0f)
			v.cvNoPreySince = now;
		const int preyIndex = keep ? engine->IndexOfEdict(prey) : -1;
		if (preyIndex != v.cvLastTarget)
		{
			v.cvLastTarget = preyIndex;
			if (keep)
				++v.cvKillsTried;
			RCBOT_DIAG("[RCBOT2] cv-hunt target userid=%d prey=%d pos=(%.0f %.0f %.0f) dist=%.0f\n", userId, preyIndex,
				origin.x, origin.y, origin.z, keep ? Dist2D(origin, CBotGlobals::entityOrigin(prey)) : -1.0f);
		}
		v.goal = v.cvGoal;
		v.goalValid = true;
	}
	edict_t* target = v.target.get();
	bool targetGone = false;
	if (target != nullptr)
	{
		const int targetIndex = engine->IndexOfEdict(target);
		if (target->IsFree())
			targetGone = true;
		else if (targetIndex <= gpGlobals->maxClients)
		{
			IPlayerInfo* info = playerinfomanager->GetPlayerInfo(target);
			// A killed soldier is not IsDead() at once (revive window): health <= 0 ends the target too
			// (human test 26.11.: MG kept firing at bodies, mg-engage hp=28->-4 and 100->-10).
			targetGone = info == nullptr || info->IsDead() || info->GetHealth() <= 0 ||
				EmpiresVehiclePlayerVehicle(this, target) >= 0;
		}
		else if (target->GetClassName() != nullptr &&
			std::strncmp(target->GetClassName(), "emp_building_", 13) == 0)
			targetGone = EmpiresVehicleReadInt(this, target, "m_iHealth", 1) <= 0;
		else
			targetGone = !EmpiresVehicleAlive(this, target);
	}
	if (targetGone)
	{
		target = nullptr;
		v.target = nullptr;
	}
	if (v.billNye && v.ramTargetIndex >= 0 && target == nullptr)
	{
		// Pushed off the map: the vehicle is removed or wrecked at once
		// (run 202417: BE CV fell with the jeep, round ended).
		RCBOT_DIAG("[RCBOT2] billnye target-gone userid=%d target=%d pos=(%.0f %.0f %.0f)\n", userId,
			v.ramTargetIndex, origin.x, origin.y, origin.z);
		v.ramTargetIndex = -1;
	}
	const bool targetIsVehicle = target != nullptr && (IsClass(target, "emp_vehicle") ||
		IsClass(target, "emp_nf_commander") || IsClass(target, "emp_imp_commander"));
	const Vector targetAt = target != nullptr ? CBotGlobals::entityOrigin(target) : Vector(0, 0, 0);

	// -------- where to drive
	Vector driveTo = v.goalValid ? v.goal : origin + YawDir(heading) * 1000.0f;
	v.fightMove = false;
	float throttle = 1.0f;
	bool ram = false;
	// The front support spot is a chosen stand: arrive close to it, then hold there.
	float arrive = v.goalKind == 2 ? 250.0f : 600.0f;
	const bool spawnApcHold = IsApcChassis(chassis) && !EmpiresLawlietIsCrew(team, client) && !v.billNye &&
		rcbot_empires_apc_cover_park.GetBool();
	if (IsApcChassis(chassis) && v.goalValid && v.goalKind == 2 && !EmpiresLawlietIsCrew(team, client))
	{
		driveTo = v.goal; // Hold the spawn route even while the gun tracks a target.
	}
	else if (spawnApcHold && target != nullptr)
	{
		// 5.14a: no spot yet: the spawn APC keeps its goal (staging) or holds, it never closes in to fight.
		driveTo = v.goalValid ? v.goal : origin;
		arrive = 250.0f;
	}
	else if (v.billNye && targetIsVehicle)
	{
		// Push toward the nearest drop from the target.
		float bestDrop = 1.0e9f;
		for (int k = 0; k < 8; ++k)
		{
			const float yaw = k * 45.0f;
			const float drop = DropDistance(targetAt, yaw, 3000.0f);
			if (drop < bestDrop)
			{
				bestDrop = drop;
				v.pushDir = YawDir(yaw);
			}
		}
		Vector fromTarget = origin - targetAt;
		fromTarget.z = 0.0f;
		const float distance = fromTarget.Length();
		const float behind = distance > 1.0f ? -DotProduct(fromTarget / distance, v.pushDir) : 0.0f;
		if (!v.ramming && (behind > 0.8f && distance < 900.0f))
			v.ramming = true;
		else if (v.ramming && (behind < 0.35f || distance > 1400.0f))
			v.ramming = false;
		if (v.ramming)
		{
			driveTo = targetAt + v.pushDir * 400.0f;
			ram = true;
			v.ramTargetIndex = engine->IndexOfEdict(target);
			arrive = 0.0f;
			if (v.contactSince == 0.0f && distance < 420.0f && speed < 60.0f)
			{
				v.contactSince = now;
				v.targetStart = targetAt;
			}
			else if (distance >= 420.0f || speed >= 60.0f)
				v.contactSince = 0.0f;
			if (v.contactSince > 0.0f && now - v.contactSince > 3.5f &&
				Dist2D(targetAt, v.targetStart) < 150.0f && now >= v.stunCooldownUntil)
			{
				RCBOT_DIAG("[RCBOT2] billnye stuck-push userid=%d target=%d -> stun\n", userId,
					engine->IndexOfEdict(target));
				v.nextAction = now;
				setStage(EmpiresVehicleStage::STUN_EXIT, 8.0f);
				return;
			}
		}
		else
		{
			driveTo = targetAt - v.pushDir * 650.0f;
			arrive = 0.0f;
		}
	}
	else if (target != nullptr && !armed)
	{
		driveTo = targetAt + (targetAt - origin).Normalized() * 200.0f; // run over
		ram = true;
		arrive = 0.0f;
	}
	else if (target != nullptr && armed && wantEmpiresRunOver(v, target, targetAt, origin, now, userId))
	{
		// Human test 26.11.: tanks never ran soldiers over. After MG fire without effect (or a soldier
		// below the gun) drive through him, like a player would, while the way there is safe.
		driveTo = targetAt + (targetAt - origin).Normalized() * 200.0f;
		ram = true;
		arrive = 0.0f;
	}
	else if (target != nullptr && armed)
	{
		// 3.21: Fight like a human tank player instead of blind high-speed circling.
		// Against stationary buildings: stop in clear sight at 550..1500 HU and shell them.
		// Against vehicles/infantry: keep moderate tactical angling (front/strong side
		// toward enemy, slower weave so cannon aim stays steady).
		const bool targetIsBuilding = target->GetClassName() != nullptr &&
			std::strncmp(target->GetClassName(), "emp_building_", 13) == 0;
		const float distance = Dist2D(origin, targetAt);
		if (HasClearFireLine(m_pEdict, vehicle, cannonEye, target))
			v.fightSight = now;
		const bool sight = now - v.fightSight < 3.0f;
		if (now < v.pitchLimitUntil && v.pitchLimitDown &&
			SafeHeading(origin, NormalizeYaw(YawTo(targetAt, origin)), 400.0f))
		{
			// Below the minimum range of the cannon (pitch clamped): open the range straight away.
			driveTo = origin + YawDir(YawTo(targetAt, origin)) * 500.0f;
			arrive = 0.0f;
			throttle = 0.55f;
			v.fightMove = true;
		}
		else if (now < v.fireBlockedFlankUntil)
		{
			// 3.26 step 4: persistent fire-blocked -> flank sideways to clear the line of fire.
			const float bearing = YawTo(origin, targetAt);
			if (v.fightDir == 0.0f)
				v.fightDir = randomFloat(0.0f, 1.0f) < 0.5f ? 1.0f : -1.0f;
			float yaw = NormalizeYaw(bearing + v.fightDir * 80.0f);
			if (!SafeHeading(origin, yaw, 700.0f))
			{
				const float other = NormalizeYaw(bearing - v.fightDir * 80.0f);
				if (SafeHeading(origin, other, 700.0f))
				{
					v.fightDir = -v.fightDir;
					yaw = other;
				}
				else
					yaw = NormalizeYaw(bearing + 180.0f);
			}
			driveTo = origin + YawDir(yaw) * 600.0f;
			arrive = 0.0f;
			throttle = 0.55f;
			v.fightMove = true;
		}
		else if (targetIsBuilding)
		{
			if (distance > 1500.0f || !sight)
			{
				driveTo = targetAt;
				arrive = sight ? 1200.0f : 300.0f;
			}
			else if (distance < 550.0f &&
				SafeHeading(origin, NormalizeYaw(YawTo(targetAt, origin)), 400.0f))
			{
				driveTo = origin + YawDir(YawTo(targetAt, origin)) * 450.0f;
				arrive = 0.0f;
				throttle = 0.45f;
				v.fightMove = true;
			}
			else
			{
				// Clean firing stand against a stationary building: stop and shell it.
				driveTo = origin;
				arrive = 200.0f;
				throttle = 0.0f;
			}
		}
		else if (distance > 1900.0f || !sight)
		{
			driveTo = targetAt;
			arrive = sight ? 1400.0f : 300.0f;
		}
		else
		{
			if (v.fightDir == 0.0f || now >= v.fightSwitch)
			{
				v.fightDir = v.fightDir == 0.0f ? (randomFloat(0.0f, 1.0f) < 0.5f ? 1.0f : -1.0f) : -v.fightDir;
				v.fightSwitch = now + randomFloat(3.0f, 6.0f);
			}
			// Far: angle in. Sweet spot (750..1400): moderate weave keeping front armor angled.
			// Against infantry, keep the front angled toward the target (avoid turning 120 deg away,
			// which blends cannon depression down to the low rear limit: +2..+3 deg).
			const int tIdx = engine->IndexOfEdict(target);
			const bool targetIsInfantry = tIdx >= 1 && tIdx <= gpGlobals->maxClients;
			const float offset = distance > 1400.0f ? 30.0f :
				(targetIsInfantry ? (distance < 550.0f ? 90.0f : 35.0f) : (distance < 750.0f ? 120.0f : 50.0f));
			const float bearing = YawTo(origin, targetAt);
			float yaw = NormalizeYaw(bearing + v.fightDir * offset);
			// Narrow road (rainbowroad +-1650): weave the way that has ground ahead.
			if (!SafeHeading(origin, yaw, 900.0f))
			{
				const float other = NormalizeYaw(bearing - v.fightDir * offset);
				if (SafeHeading(origin, other, 900.0f))
				{
					v.fightDir = -v.fightDir;
					yaw = other;
				}
				else
					yaw = bearing; // both sides drop off: straight at the target
			}
			driveTo = origin + YawDir(yaw) * 600.0f;
			arrive = 0.0f;
			throttle = distance > 1400.0f ? 0.6f : 0.45f;
			v.fightMove = true;
			if (now >= v.nextFightLog)
			{
				v.nextFightLog = now + 6.0f;
				RCBOT_DIAG("[RCBOT2] vehicle fight-move userid=%d target=%d dist=%.0f dir=%.0f offset=%.0f\n", userId,
					engine->IndexOfEdict(target), distance, v.fightDir, offset);
			}
		}
	}

	// Fair test start (rcbot_empires_vehicle_hold): wait where bought until both teams are ready.
	// Mode 2 also holds stationary while firing (controlled range tests).
	if ((rcbot_empires_vehicle_hold.GetInt() >= 2 || (rcbot_empires_vehicle_hold.GetBool() && target == nullptr)) &&
		!v.commanderCV)
	{
		driveTo = origin;
		arrive = 200.0f;
		throttle = 0.0f;
		v.fightMove = false;
	}

	// TODO 8.4: advance together. On the way to the front a tank with an escorting engineer waits while
	// it is more than 1500 HU ahead (until < 800), at most 15 s, then drives on for at least 30 s.
	if (rcbot_empires_tank_escort.GetBool() && target == nullptr && !v.commanderCV && !v.retreating &&
		(v.goalKind == 2 || v.goalKind == 3 || v.goalKind == 4) && team >= 0 && team < 4)
	{
		const EscortClaim& claim = s_escortClaim[team];
		edict_t* escort = nullptr;
		if (claim.vehicle == v.vehicleIndex && now < claim.until)
			for (int i = 1; i <= gpGlobals->maxClients && escort == nullptr; ++i)
			{
				edict_t* p = engine->PEntityOfEntIndex(i);
				if (p != nullptr && !p->IsFree() && engine->GetPlayerUserId(p) == claim.engineerUserId &&
					CBotGlobals::entityIsAlive(p))
					escort = p;
			}
		const float gap = escort != nullptr ? Dist2D(origin, CBotGlobals::entityOrigin(escort)) : 0.0f;
		const RepairRequest& rr = s_repairRequest[team];
		const bool repairCalled = escort != nullptr && rr.vehicle == v.vehicleIndex && now <= rr.until &&
			rr.engineerUserId == claim.engineerUserId;
		if (escort == nullptr || (v.escortWaitSince > 0.0f && gap < (repairCalled ? 600.0f : 800.0f)))
		{
			if (v.escortWaitSince > 0.0f)
				RCBOT_DIAG("[RCBOT2] tank-escort wait-end userid=%d veh=%d gap=%.0f waited=%.1f\n", userId,
					v.vehicleIndex, gap, now - v.escortWaitSince);
			v.escortWaitSince = 0.0f;
		}
		else if (v.escortWaitSince > 0.0f && now - v.escortWaitSince > 15.0f)
		{
			RCBOT_DIAG("[RCBOT2] stall-escalate who=tank-escort-wait stage=1 reason=escort-slow veh=%d gap=%.0f\n",
				v.vehicleIndex, gap);
			v.escortWaitSince = 0.0f;
			v.escortWaitBlockedUntil = now + 30.0f;
		}
		else if (v.escortWaitSince == 0.0f && gap > 1500.0f && now >= v.escortWaitBlockedUntil)
		{
			v.escortWaitSince = now;
			RCBOT_DIAG("[RCBOT2] tank-escort wait userid=%d veh=%d gap=%.0f\n", userId, v.vehicleIndex, gap);
		}
		// The escort is repairing this tank (its call stands): hold still, at most 30 s.
		const bool beingRepaired = repairCalled && gap < (v.escortRepairHoldSince > 0.0f ? 900.0f : 600.0f);
		if (beingRepaired && v.escortRepairHoldSince == 0.0f)
		{
			v.escortRepairHoldSince = now;
			RCBOT_DIAG("[RCBOT2] tank-escort repair-hold userid=%d veh=%d gap=%.0f\n", userId, v.vehicleIndex, gap);
		}
		else if (!beingRepaired && v.escortRepairHoldSince > 0.0f)
		{
			RCBOT_DIAG("[RCBOT2] tank-escort repair-hold-end userid=%d veh=%d held=%.1f req_veh=%d req_left=%.1f req_eng=%d escort=%d gap=%.0f\n",
				userId, v.vehicleIndex, now - v.escortRepairHoldSince, rr.vehicle, rr.until - now, rr.engineerUserId,
				claim.engineerUserId, gap);
			v.escortRepairHoldSince = 0.0f;
		}
		if (v.escortWaitSince > 0.0f || (v.escortRepairHoldSince > 0.0f && now - v.escortRepairHoldSince < 30.0f))
		{
			driveTo = origin;
			arrive = 200.0f;
			throttle = 0.0f;
		}
	}
	else
	{
		v.escortWaitSince = 0.0f;
		v.escortRepairHoldSince = 0.0f;
	}

	Vector lawlietDriveTo = driveTo;
	float lawlietArrive = arrive;
	float lawlietThrottle = throttle;
	bool lawlietHold = false;
	const bool lawlietDrive = client > 0 && EmpiresLawlietOverrideDriving(
		team, client, v.vehicleIndex, lawlietDriveTo, lawlietArrive, lawlietThrottle, lawlietHold);
	if (lawlietDrive)
		v.retreating = false;

	// -------- badly damaged: back to a repair station or the base
	// Hull (m_iHealth) and, with armor, each side separately: one side shot
	// through is as bad as a low hull.
	float health = -1.0f;
	ReadVehicleHealth(vehicle, health);
	if (health > v.maxHealth)
		v.maxHealth = health;
	int armor[4] = {-1, -1, -1, -1};
	int armorMax = EmpiresVehicleReadInt(this, vehicle, "m_iMaxArmorHealth", 0);
	float worstArmor = 1.0f;
	int worstSide = -1;
	float bestArmor = -1.0f;
	int bestSide = -1;
	float total = (std::max)(0.0f, health);
	for (int k = 0; k < 4; ++k)
	{
		armor[k] = ReadArmorSide(vehicle, k);
		if (armor[k] > v.armorMaxSeen[k])
			v.armorMaxSeen[k] = armor[k];
		// m_iMaxArmorHealth is per inch (70; 2 inch sides read 140, run 222443),
		// and armor builds up after buying (read 0/0/0/0, run 231637): judge a
		// side only against what it already had.
		const int sideMax = v.armorMaxSeen[k];
		if (armor[k] < 0 || sideMax <= 0)
			continue;
		total += armor[k];
		const float frac = float(armor[k]) / float(sideMax);
		if (frac < worstArmor)
		{
			worstArmor = frac;
			worstSide = k;
		}
		if (frac > bestArmor)
		{
			bestArmor = frac;
			bestSide = k;
		}
	}
	v.worstSide = worstSide;
	if (health > 0.0f && v.maxHealth > 0.0f)
	{
		const float hullFrac = health / v.maxHealth;
		// One weak side: turn a strong one to the enemy (below). Back only when
		// every side is nearly through or the hull is low.
		const bool armorGone = bestSide >= 0 && bestArmor < 0.3f;
		// 3.10d / 3.10b: an MG sharing its button with the cannon (adopted vehicle, other loadout)
		// cannot be used on infantry (mg-mixed), or heavy weapons are empty; refit/rearm on a pad
		// (or crate/ammo-call). Supports all refittable tank chassis (Light/AFV, Medium, Heavy).
		if (!lawlietDrive && !v.retreating && !v.billNye && !v.commanderCV &&
			EmpiresVehicleLoadout::IsRefittableTankChassis(chassis) &&
			now >= v.loadoutCheckAt && (v.loadoutVerify || (now >= v.loadoutRetryAt && now >= v.retreatCooldownUntil)))
		{
			v.loadoutCheckAt = now + 3.0f;
			int turretIndex = -1;
			edict_t* turret = readEmpiresEntityHandleProperty(vehicle, "m_hTurret", 0, turretIndex) &&
				turretIndex > 0 && turretIndex < gpGlobals->maxEntities ? engine->PEntityOfEntIndex(turretIndex) : nullptr;
			ReadVehicleWeapons(this, turret, v);
			v.weaponReadAt = now;
			if (v.loadoutVerify)
			{
				v.loadoutVerify = false;
				RCBOT_DIAG("[RCBOT2] vehicle loadout-check userid=%d veh=%d known=%d mixed=%d mg=%d heavy-left=%d "
					"group=%d groups=%d weapons=%s\n",
					userId, v.vehicleIndex, v.weaponsKnown ? 1 : 0, v.weaponMgMixed, v.weaponMgPure,
					v.weaponHeavyLeft ? 1 : 0, v.weaponGroup + 1, v.weaponGroupCount, v.weaponSummary);
			}
			else if (v.weaponsKnown && (v.weaponMgMixed != 0 || (v.weaponHeavyPresent && !v.weaponHeavyLeft)))
			{
				// 3.10b step 1: cannon/missiles out of magazine and reserve: the refit on the pad
				// refills every weapon (AddWeapon 0x69a090 sets magazine and reserve to max, RE).
				v.ammoTrip = v.weaponMgMixed == 0;
				v.crateIndex = v.crateSkip = -1;
				v.crateDone = false;
				v.retreating = true;
				v.loadoutRefit = true;
				v.lastHealth = total;
				v.lastHealthRise = now;
				v.nextReplan = 0.0f;
				v.parkCount = 0;
				v.padEntryAttempt = 0;
				RCBOT_DIAG("[RCBOT2] vehicle retreat userid=%d veh=%d reason=%s weapons=%s pos=(%.0f %.0f %.0f)\n",
					userId, v.vehicleIndex, v.ammoTrip ? "ammo" : "loadout", v.weaponSummary, origin.x, origin.y, origin.z);
			}
		}
		const bool spawnApcHull = IsApcChassis(chassis) && !EmpiresLawlietIsCrew(team, client);
		const float retreatHull = spawnApcHull ? rcbot_empires_apc_retreat_hull.GetFloat() : 0.4f;
		// 5.14a: a park spot under fire (hit from out of sight: turrets, mortars, rockets) is given up.
		if (spawnApcHull && !v.retreating && v.goalValid && v.goalKind == 2 && v.supportKnown &&
			Dist2D(origin, v.supportSpot) < 300.0f)
		{
			if (v.parkArriveAt <= 0.0f || now - v.parkArriveAt > 15.0f)
			{
				v.parkArriveAt = now;
				v.parkArriveTotal = total;
			}
			else if (v.parkArriveTotal > 0.0f && total < v.parkArriveTotal * 0.85f)
			{
				RCBOT_DIAG("[RCBOT2] stall-escalate who=apc-park stage=1 reason=damaged lost=%.0f/%.0f in=%.0fs spot=(%.0f %.0f %.0f)\n",
					v.parkArriveTotal - total, v.parkArriveTotal, now - v.parkArriveAt, v.supportSpot.x, v.supportSpot.y,
					v.supportSpot.z);
				v.rejectedPark = v.supportSpot;
				v.rejectedParkUntil = now + 45.0f;
				v.supportKnown = false;
				v.nextParkSearch = 0.0f;
				v.parkArriveAt = 0.0f;
			}
		}
		else
			v.parkArriveAt = 0.0f;
		if (!lawlietDrive && !v.retreating && !v.billNye && !v.commanderCV && (hullFrac < retreatHull || armorGone) &&
			now >= v.retreatCooldownUntil)
		{
			v.retreating = true;
			v.lastHealth = total;
			v.lastHealthRise = now;
			v.nextReplan = 0.0f;
			v.parkCount = 0;
			v.padEntryAttempt = 0;
			v.padStalls = 0;
			v.padGiveUp = false;
			RCBOT_DIAG("[RCBOT2] vehicle retreat userid=%d veh=%d hp=%.0f/%.0f armor=%d/%d/%d/%d max=%d worst=%d "
				"pos=(%.0f %.0f %.0f)\n", userId, v.vehicleIndex, health, v.maxHealth, armor[0], armor[1],
				armor[2], armor[3], armorMax, worstSide, origin.x, origin.y, origin.z);
		}
		if (v.retreating)
		{
			if (total > v.lastHealth + 0.5f)
				v.lastHealthRise = now;
			v.lastHealth = total;
			// An engineer on the way keeps the wait alive (run 214309: walked
			// 16000 units, the request expired at 10000).
			const RepairRequest& req = s_repairRequest[team];
			if (req.vehicle == v.vehicleIndex && req.engineerUserId >= 0 && now <= req.claimUntil)
				v.lastHealthRise = (std::max)(v.lastHealthRise, now - 20.0f);
			// The hull may stop short of the highest value seen (run 220710:
			// 140/150, healed while waiting): a long plateau high up counts too.
			const bool plateau = now - v.lastHealthRise > 30.0f && hullFrac >= 0.8f && worstArmor >= 0.8f;
			// Damage trips can reach full HP between selecting the engine and sending the build.
			// Allow an active sequence to finish, but release a healed tank if it loses the pad.
			const bool activeRefit = v.refitStep > 0 && now <= v.refitNext + 2.0f;
			const bool refitPending = !v.refitDone && (v.loadoutRefit || activeRefit);
			const char* end = v.crateDone ? "resupplied" : refitPending ? (now - v.lastHealthRise > 180.0f ? "retreat-timeout" : nullptr) : (hullFrac >= 0.95f && worstArmor >= 0.9f) || plateau ? "repaired" :
				now - v.lastHealthRise > 120.0f ? "retreat-timeout" : nullptr;
			// Never park for good (user): after 20 s without repairs try again
			// (the stuck edge is blocked for vehicles, the other pad entrance
			// next); after three parks give the retreat up and fight on.
			if (end == nullptr && v.padGiveUp)
				end = "retreat-abort";
			if (end == nullptr && v.parkValid && now - v.parkSince > 20.0f && now - v.lastHealthRise > 8.0f)
			{
				if (v.parkCount >= 3)
					end = "retreat-abort";
				else
				{
					v.parkValid = false;
					v.stuckReverses = 0;
					v.nextReplan = 0.0f;
					v.padStation = -1;
					++v.padEntryAttempt;
					// A crate the vehicle cannot reach: try the pad next (expert review).
					if (v.crateIndex >= 0)
					{
						v.crateSkip = v.crateIndex;
						v.crateIndex = -1;
					}
					RCBOT_DIAG("[RCBOT2] vehicle park-retry userid=%d veh=%d parks=%d pos=(%.0f %.0f)\n", userId,
						v.vehicleIndex, v.parkCount, origin.x, origin.y);
				}
			}
			if (end != nullptr)
			{
				RCBOT_DIAG("[RCBOT2] vehicle %s userid=%d veh=%d hp=%.0f/%.0f armor=%d/%d/%d/%d max=%d refit_sent=%d\n", end, userId,
					v.vehicleIndex, health, v.maxHealth, armor[0], armor[1], armor[2], armor[3], armorMax, v.refitDone ? 1 : 0);
				v.retreating = false;
				v.parkValid = false;
				v.homeParkValid = false;
				v.padGiveUp = false;
				v.padStalls = 0;
				if (v.loadoutRefit)
				{
					// Read the loadout again soon; a refused refit (money) is not retried for 2 min.
					v.loadoutCheckAt = now + 2.0f;
					// A crate refill succeeded: a new empty cannon may start the next trip soon (expert review).
					v.loadoutRetryAt = now + (v.crateDone ? 10.0f : 120.0f);
					v.loadoutVerify = true;
				}
				const bool wasLoadout = v.loadoutRefit;
				v.loadoutRefit = false;
				v.ammoTrip = false;
				v.crateIndex = -1;
				v.crateDone = false;
				v.refitDone = false;
				v.refitStep = 0;
				v.padStation = -1;
				v.padInside = false;
				v.nextReplan = 0.0f;
				// A failed loadout trip is held off by loadoutRetryAt; damage retreats stay allowed.
				if (std::strcmp(end, "repaired") != 0 && !wasLoadout)
					v.retreatCooldownUntil = now + 60.0f;
				if (s_repairRequest[team].vehicle == v.vehicleIndex)
					s_repairRequest[team] = RepairRequest();
				EmpiresAmmoRequestClear(team, v.vehicleIndex);
			}
		}
	}
	bool drivingToPad = false;
	if (v.retreating)
	{
		// Nearest own, finished repair station, else the own command vehicle.
		// The class is emp_building_repairstation for both teams and its team
		// reads -1 (run 232838): without a team, it belongs to the side whose
		// CV is nearer.
		const char* cvClass = team == 2 ? "emp_nf_commander" : "emp_imp_commander";
		const char* enemyCvClass = team == 2 ? "emp_imp_commander" : "emp_nf_commander";
		edict_t* station = nullptr;
		Vector home(0, 0, 0), enemyHome(0, 0, 0);
		bool haveHome = false, haveEnemyHome = false;
		std::vector<edict_t*> stations;
		std::vector<edict_t*> crates;
		const char* mapCrateClass = team == 2 ? "emp_nf_ammo_crate" : "emp_imp_ammo_crate";
		for (int i = gpGlobals->maxClients + 1; i < gpGlobals->maxEntities; ++i)
		{
			edict_t* e = engine->PEntityOfEntIndex(i);
			if (e == nullptr || e->IsFree() || e->GetClassName() == nullptr)
				continue;
			// 3.10b step 2: engineer crates and the team's map ammo crates feed vehicles within 1000
			// every second (CEngineerCrate::CrateThink 0x62af30); health crates do not.
			if (v.ammoTrip && i != v.crateSkip &&
				((IsClass(e, "emp_eng_crate") && EmpiresVehicleReadInt(this, e, "m_iTeamNum", -1) == team) ||
					IsClass(e, mapCrateClass)))
				crates.push_back(e);
			else if (std::strstr(e->GetClassName(), "repairstation") != nullptr &&
				std::strncmp(e->GetClassName(), "emp_building_", 13) == 0)
				stations.push_back(e);
			else if (!haveHome && IsClass(e, cvClass))
			{
				home = CBotGlobals::entityOrigin(e);
				haveHome = true;
			}
			else if (!haveEnemyHome && IsClass(e, enemyCvClass))
			{
				enemyHome = CBotGlobals::entityOrigin(e);
				haveEnemyHome = true;
			}
		}
		float best = 1.0e9f;
		for (edict_t* e : stations)
		{
			const Vector at = CBotGlobals::entityOrigin(e);
			const int stationTeam = EmpiresVehicleReadInt(this, e, "m_iTeamNum", -1);
			const bool ours = stationTeam == team ||
				(stationTeam != 2 && stationTeam != 3 && haveHome &&
					(!haveEnemyHome || Dist2D(at, home) < Dist2D(at, enemyHome)));
			const int hp = EmpiresVehicleReadInt(this, e, "m_iHealth", 0);
			const int hpMax = EmpiresVehicleReadInt(this, e, "m_iMaxHealth", 0);
			if (!ours || hp <= 0 || (hpMax > 0 && hp < hpMax))
				continue;
			const float d = Dist2D(origin, at);
			if (d < best)
			{
				best = d;
				station = e;
			}
		}
		// Ammo only: a crate nearer than the pad (or no pad) is enough; not once the refit has begun.
		edict_t* crate = nullptr;
		float crateDist = 1.0e9f;
		// An engineer on the way to our own call: only a near crate beats waiting for him (run
		// 20261115-014937: the second tank drove 13000 to the first tank's crate, its engineer gave up).
		const EmpiresAmmoRequest* heldCall = v.ammoTrip ? EmpiresAmmoRequestForVehicle(team, v.vehicleIndex, false) : nullptr;
		const float crateRange = heldCall != nullptr && heldCall->engineerUserId >= 0 && now <= heldCall->claimUntil ?
			2500.0f : 1.0e9f;
		if (v.ammoTrip && v.refitStep == 0)
			for (edict_t* e : crates)
			{
				const float d = Dist2D(origin, CBotGlobals::entityOrigin(e));
				if (d < crateDist && d <= crateRange && (station == nullptr || d < best))
				{
					crateDist = d;
					crate = e;
				}
			}
		if (crate == nullptr && v.crateIndex >= 0)
		{
			RCBOT_DIAG("[RCBOT2] vehicle crate-lost userid=%d veh=%d crate=%d pad=%d\n", userId, v.vehicleIndex,
				v.crateIndex, station != nullptr ? engine->IndexOfEdict(station) : -1);
			v.crateIndex = -1;
			v.nextReplan = 0.0f;
		}
		// 3.10b step 3: no crate and no pad within 2500: call for ammo; an engineer who took the
		// call (claim refreshed every frame) is waited for where the tank stands.
		bool ammoWait = false;
		if (v.ammoTrip && crate == nullptr && v.refitStep == 0 && (station == nullptr || best > 2500.0f))
		{
			const bool call = now >= v.nextAmmoCall;
			EmpiresAmmoRequest* slot = EmpiresAmmoRequestForVehicle(team, v.vehicleIndex, call);
			if (call && slot != nullptr)
			{
				EmpiresAmmoRequest& ar = *slot;
				v.nextAmmoCall = now + 25.0f;
				ar.driverUserId = userId;
				ar.until = now + 40.0f;
				if (helpers != nullptr)
					helpers->ClientCommand(m_pEdict, "emp_menu_command 29 base\n");
				RCBOT_DIAG("[RCBOT2] vehicle ammo-call userid=%d veh=%d pad=%.0f pos=(%.0f %.0f %.0f)\n", userId,
					v.vehicleIndex, station != nullptr ? best : -1.0f, origin.x, origin.y, origin.z);
			}
			if (slot != nullptr)
			{
				slot->until = (std::max)(slot->until, now + 5.0f);
				ammoWait = slot->engineerUserId >= 0 && now <= slot->claimUntil;
			}
		}
		else
			EmpiresAmmoRequestClear(team, v.vehicleIndex); // a crate or a near pad answers the call
		if (ammoWait && !v.parkValid)
		{
			driveTo = origin;
			arrive = 400.0f;
			// Waiting counts as progress (trip timeout, no-pad abort after 30 s).
			v.lastHealthRise = (std::max)(v.lastHealthRise, now - 20.0f);
		}
		else if (v.parkValid)
		{
			// Stuck on the way home (base gates/ramps only open from inside,
			// run 214613): wait where backing off ended, not at the obstacle
			// (engineers could not reach every side there, run 223745).
			driveTo = origin;
			arrive = 400.0f;
		}
		else if (crate != nullptr)
		{
			const Vector at = CBotGlobals::entityOrigin(crate);
			const int crateIndex = engine->IndexOfEdict(crate);
			if (v.crateIndex != crateIndex)
			{
				v.crateIndex = crateIndex;
				v.crateNearSince = -1.0f;
				v.crateAmmoSeen = -1;
				v.nextReplan = 0.0f;
				v.padStation = -1; // a later pad approach picks its entrance again
				v.padInside = false;
				RCBOT_DIAG("[RCBOT2] vehicle crate-target userid=%d veh=%d crate=%d class=%s dist=%.0f pad=%.0f stations=%zu\n", userId,
					v.vehicleIndex, crateIndex, crate->GetClassName(), crateDist, station != nullptr ? best : -1.0f, stations.size());
			}
			driveTo = at;
			arrive = 450.0f;
			// The crate measures from the vehicle's WorldSpaceCenter (1000); 850 leaves a margin.
			if ((origin - at).Length() < 850.0f)
			{
				if (v.crateNearSince < 0.0f)
				{
					v.crateNearSince = now;
					v.crateAmmoRise = now;
					RCBOT_DIAG("[RCBOT2] vehicle crate-reach userid=%d veh=%d crate=%d dist=%.0f weapons=%s\n", userId,
						v.vehicleIndex, crateIndex, (origin - at).Length(), v.weaponSummary);
				}
				if (now >= v.crateReadAt)
				{
					v.crateReadAt = now + 1.0f;
					int turretIndex = -1;
					edict_t* turret = readEmpiresEntityHandleProperty(vehicle, "m_hTurret", 0, turretIndex) &&
						turretIndex > 0 && turretIndex < gpGlobals->maxEntities ? engine->PEntityOfEntIndex(turretIndex) : nullptr;
					ReadVehicleWeapons(this, turret, v);
					v.weaponReadAt = now;
					if (v.weaponHeavyTotal > v.crateAmmoSeen)
					{
						v.crateAmmoSeen = v.weaponHeavyTotal;
						v.crateAmmoRise = now;
					}
				}
				// Full when nothing came for 8 s (GiveAmmo steps every reload * 0.5 s, RE).
				if (v.weaponsKnown && v.weaponHeavyLeft && now - v.crateAmmoRise > 8.0f)
				{
					v.crateDone = true;
					RCBOT_DIAG("[RCBOT2] vehicle crate-done userid=%d veh=%d crate=%d waited=%.0f weapons=%s\n", userId,
						v.vehicleIndex, crateIndex, now - v.crateNearSince, v.weaponSummary);
				}
				else if (now - v.crateAmmoRise > 30.0f)
				{
					RCBOT_DIAG("[RCBOT2] vehicle crate-skip userid=%d veh=%d crate=%d weapons=%s\n", userId,
						v.vehicleIndex, crateIndex, v.weaponSummary);
					v.crateSkip = crateIndex;
					v.crateIndex = -1;
					v.nextReplan = 0.0f;
				}
			}
		}
		else if (station != nullptr)
		{
			// Onto the pad: repairs reach 600, but refitting (emp_vehicle_build
			// while driving, Command_emp_vehicle_build 0x654750) traces for the
			// station under the vehicle.
			const Vector pad = CBotGlobals::entityOrigin(station);
			const int stationIndex = engine->IndexOfEdict(station);
			if (v.padStation != stationIndex)
			{
				v.padStation = stationIndex;
				v.padProgressSince = -1.0f;
				v.padInside = false;
				char why[512];
				const std::vector<Vector> entries = RepairPadEntrances(station, origin.z, why, sizeof(why));
				// Shortest vehicle route first (station next to the factory: the
				// nearer entrance by air sat behind the gap between both, run
				// 20261112-120808); after each failed try the next one.
				std::vector<std::pair<float, Vector>> ranked;
				for (const Vector& entry : entries)
				{
					float length = Dist2D(origin, entry) * 3.0f;
					if (RCBotNavMesh::IsLoaded())
					{
						RCBotNavMesh::Path route;
						if (RCBotNavMesh::BuildVehiclePath(origin, entry, route, v.width > 0.0f ? v.width : 150.0f, false, v.waterDepth, vehicle) ==
							RCBotNavMesh::PathResult::SUCCESS)
							length = route.length;
					}
					ranked.push_back({length, entry});
				}
				std::sort(ranked.begin(), ranked.end(),
					[](const std::pair<float, Vector>& a, const std::pair<float, Vector>& b) { return a.first < b.first; });
				std::vector<Vector> sorted;
				for (const auto& r : ranked)
					sorted.push_back(r.second);
				v.padEntryValid = !sorted.empty();
				if (v.padEntryValid)
					v.padEntry = sorted[static_cast<size_t>(v.padEntryAttempt) % sorted.size()];
				RCBOT_DIAG("[RCBOT2] vehicle pad-entry userid=%d station=%d open=%zu entry=(%.0f %.0f) route=%.0f/%.0f try=%d %s\n",
					userId, stationIndex, entries.size(), v.padEntry.x, v.padEntry.y,
					ranked.empty() ? -1.0f : ranked[0].first, ranked.size() > 1 ? ranked[1].first : -1.0f,
					v.padEntryAttempt, why);
			}
			drivingToPad = Dist2D(origin, pad) >= 140.0f;
			if (v.padEntryValid && !v.padInside && Dist2D(origin, v.padEntry) < 250.0f)
			{
				v.padInside = true;
				v.nextReplan = 0.0f;
				RCBOT_DIAG("[RCBOT2] vehicle pad-enter userid=%d pos=(%.0f %.0f)\n", userId, origin.x, origin.y);
			}
			if (v.padEntryValid && !v.padInside)
			{
				driveTo = v.padEntry;
				arrive = 150.0f;
			}
			else
			{
				driveTo = pad;
				arrive = 100.0f;
				// On the pad follow its axis (entry -> centre), aiming 350 ahead on
				// the centre line: straight at the centre from a point beside the
				// entry the tank hit the station building on the pad's side (run
				// 20261112-125839).
				if (v.padEntryValid)
				{
					Vector axis = v.padEntry - pad;
					axis.z = 0.0f;
					const float len = axis.Length2D();
					if (len > 1.0f)
					{
						axis /= len;
						const float along = (origin.x - pad.x) * axis.x + (origin.y - pad.y) * axis.y;
						if (along > 150.0f)
						{
							driveTo = pad + axis * (std::max)(0.0f, along - 350.0f);
							driveTo.z = pad.z;
						}
					}
				}
			}
			if (!v.refitDone && Dist2D(origin, pad) < 140.0f && std::fabs(forwardSpeed) < 60.0f &&
				now >= v.refitNext && helpers != nullptr)
			{
				const bool nf = team == 2;
				char command[96] = "";
				// Same loadout as bought: the game charges only the difference to the vehicle's
				// total cost (0x6550a7), so this refit is free and needs no reserve check
				// (run 214852: pool unchanged, armor 14 -> 140; RE vehicle-driving.md).
				switch (v.refitStep)
				{
				case 0:
				{
					bool researchDone[EmpiresCommanderEconomy::kResearchItemCount] = {};
					const bool haveResearch = EmpiresReadTeamResearch(team, researchDone);
					float budget = -1.0f;
					float pool = -1.0f;
					if (EmpiresTeamResources(team, pool))
					{
						const float wages = (std::max)(0.0f, EmpiresPlayerWages(m_pEdict));
						const float reserve = EmpiresTeamEconomy::ActiveReserve(
							EmpiresTeamEconomy::Team(team), gpGlobals->curtime);
						budget = wages + (std::max)(0.0f, pool - reserve);
					}
					// 3.10f: keep a missile launcher the vehicle carries; add one only on an
					// upgrade refit with money for it.
					bool withMissile = false;
					if (rcbot_empires_vehicle_missiles.GetBool())
					{
						int turretIndex = -1;
						edict_t* turret = readEmpiresEntityHandleProperty(vehicle, "m_hTurret", 0, turretIndex) &&
							turretIndex > 0 && turretIndex < gpGlobals->maxEntities ?
							engine->PEntityOfEntIndex(turretIndex) : nullptr;
						withMissile = VehicleCarriesMissile(turret) ||
							(!v.ammoTrip && (budget < 0.0f || budget >= EmpiresVehicleLoadout::kMissileCost));
					}
					v.refitLoadout = EmpiresVehicleLoadout::ChooseRefitLoadout(
						nf, chassis, haveResearch ? researchDone : nullptr, !v.ammoTrip, budget, withMissile);
					snprintf(command, sizeof(command), "emp_vehicle_chassis %d\n", v.refitLoadout.chassis);
					break;
				}
				case 1:
					if (!v.refitLoadout.isJeep() && v.refitLoadout.cannonSlot >= 0)
						snprintf(command, sizeof(command), "emp_vehicle_weapon %d 0 %d 1 %d 0 0 0 0 1\n",
							v.refitLoadout.cannonSlot, v.refitLoadout.cannonWeapon,
							v.refitLoadout.missileSlot >= 0 ? 1 : 0);
					break;
				case 2:
					if (!v.refitLoadout.isJeep() && v.refitLoadout.mgSlot >= 0)
						snprintf(command, sizeof(command), "emp_vehicle_weapon %d 0 %d 1 %d 0 0 0 0 0\n",
							v.refitLoadout.mgSlot, v.refitLoadout.mgWeapon,
							v.refitLoadout.missileSlot >= 0 ? 1 : 0);
					if (!v.refitLoadout.isJeep() && v.refitLoadout.missileSlot >= 0)
					{
						char missile[96];
						snprintf(missile, sizeof(missile), "emp_vehicle_weapon %d 0 %d 1 0 0 0 0 0 1\n",
							v.refitLoadout.missileSlot, v.refitLoadout.missileWeapon);
						helpers->ClientCommand(m_pEdict, missile);
						RCBOT_DIAG("[RCBOT2] vehicle refit userid=%d veh=%d step=%d cmd=%s", userId,
							v.vehicleIndex, v.refitStep, missile);
					}
					break;
				case 3:
					snprintf(command, sizeof(command), "emp_vehicle_armor 0 %d\n", v.refitLoadout.armorType);
					break;
				case 4:
					snprintf(command, sizeof(command), "emp_vehicle_armor_levels %d %d %d %d\n",
						v.refitLoadout.armorLevels[0], v.refitLoadout.armorLevels[1],
						v.refitLoadout.armorLevels[2], v.refitLoadout.armorLevels[3]);
					break;
				case 5:
					snprintf(command, sizeof(command), "emp_vehicle_engine 0 %d\n", v.refitLoadout.engine);
					break;
				case 6: snprintf(command, sizeof(command), "emp_vehicle_build\n"); break;
				default: break;
				}
				if (command[0] != '\0')
				{
					helpers->ClientCommand(m_pEdict, command);
					RCBOT_DIAG("[RCBOT2] vehicle refit userid=%d veh=%d step=%d cmd=%s", userId, v.vehicleIndex,
						v.refitStep, command);
				}
				v.refitNext = now + 0.3f;
				if (++v.refitStep > 6)
				{
					v.refitDone = true;
					RCBOT_DIAG("[RCBOT2] vehicle refit-sent userid=%d veh=%d hp=%.0f armor=%d/%d/%d/%d pad=%.0f\n", userId,
						v.vehicleIndex, health, armor[0], armor[1], armor[2], armor[3], Dist2D(origin, pad));
				}
			}
		}
		else if (v.loadoutRefit && (!haveHome || now - v.lastHealthRise > (v.ammoTrip ? 60.0f : 30.0f)))
		{
			EmpiresAmmoRequestClear(team, v.vehicleIndex);
			// No finished own pad for 30 s (a new one may still be building: head home meanwhile,
			// run 20261114-2216): the loadout cannot be changed, fight on and look again later.
			v.retreating = false;
			v.loadoutRefit = false;
			v.ammoTrip = false;
			v.crateIndex = -1;
			v.loadoutRetryAt = now + 60.0f;
			v.nextReplan = 0.0f;
			// Pad lost mid-refit: start the next refit from step 0 (expert review).
			v.refitDone = false;
			v.refitStep = 0;
			v.padStation = -1;
			v.padInside = false;
			v.homeParkValid = false;
			RCBOT_DIAG("[RCBOT2] vehicle retreat-abort userid=%d veh=%d reason=loadout-no-pad\n", userId, v.vehicleIndex);
		}
		else if (haveHome)
		{
			// Park in front of the CV, not on it: an engineer's stand spot
			// inside the CV box has no route (run 215310, nav-detour no-exit).
			if (!v.homeParkValid || Dist2D(v.homeParkFor, home) > 300.0f)
			{
				Vector away = origin - home;
				away.z = 0.0f;
				away = away.Length() > 1.0f ? away.Normalized() : Vector(1, 0, 0);
				v.homeParkValid = false;
				// The side towards the vehicle is chosen far from base; there a building may stand
				// (Coast BE, run 20261120-152144: spot behind the barracks, 2-point route through it,
				// give-up). Turn around the CV in 45° steps to the first free, reachable spot.
				for (int k = 0; k < 8; ++k)
				{
					const float a = static_cast<float>((k + 1) / 2) * (k % 2 ? 0.785398f : -0.785398f);
					const Vector dir(away.x * std::cos(a) - away.y * std::sin(a),
						away.x * std::sin(a) + away.y * std::cos(a), 0.0f);
					Vector candidate = home + dir * 700.0f;
					if (!RCBotNavMesh::ClosestPointOnMesh(candidate, 300.0f, candidate))
						continue;
					RCBotNavMesh::Path path;
					if (RCBotNavMesh::PointNearBuilding(candidate, 200.0f) ||
						RCBotNavMesh::BuildVehiclePath(origin, candidate, path, v.width, false, v.waterDepth) !=
							RCBotNavMesh::PathResult::SUCCESS ||
						path.points.empty() || (path.points.back() - candidate).Length() > 250.0f)
						continue;
					v.homePark = candidate;
					v.homeParkFor = home;
					v.homeParkValid = true;
					RCBOT_DIAG("[RCBOT2] vehicle home-park userid=%d veh=%d spot=(%.0f %.0f %.0f) cv=(%.0f %.0f) turn=%d\n",
						userId, v.vehicleIndex, candidate.x, candidate.y, candidate.z, home.x, home.y, k);
					break;
				}
				if (!v.homeParkValid)
				{
					v.parkValid = true;
					v.park = origin;
					v.parkSince = now;
					++v.parkCount;
					RCBOT_DIAG("[RCBOT2] stall-escalate who=vehicle stage=1 reason=no-home-park userid=%d veh=%d parks=%d\n",
						userId, v.vehicleIndex, v.parkCount);
				}
			}
			driveTo = v.homeParkValid ? v.homePark : origin;
			arrive = 250.0f;
		}
		ram = false;
		throttle = 1.0f;
		const bool arrived = (v.parkValid || station != nullptr || haveHome) &&
			Dist2D(origin, driveTo) < arrive + 200.0f;
		// Engineers only without a finished repair pad (user): with a pad the
		// tank repairs and refits there; only if it got stuck on the way.
		if (arrived && (v.parkValid || station == nullptr) && !v.loadoutRefit &&
			now >= v.nextRepairCall)
		{
			v.nextRepairCall = now + 25.0f;
			RepairRequest& r = s_repairRequest[team];
			if (r.vehicle != v.vehicleIndex && r.human && RepairRequestVehicle(team) != nullptr &&
				rcbot_empires_humans_first.GetBool())
			{
				// TODO 8.3: a human vehicle is being repaired; call again a bit later.
				v.nextRepairCall = now + 8.0f;
				RCBOT_DIAG("[RCBOT2] vehicle repair-call-wait userid=%d veh=%d human_veh=%d\n", userId, v.vehicleIndex, r.vehicle);
			}
			else
			{
				if (r.vehicle != v.vehicleIndex)
					r = RepairRequest();
				r.vehicle = v.vehicleIndex;
				r.driverUserId = userId;
				r.origin = origin;
				r.until = now + 30.0f;
				r.side = v.worstSide;
				if (helpers != nullptr)
					helpers->ClientCommand(m_pEdict, "emp_menu_command 30 base\n");
				RCBOT_DIAG("[RCBOT2] vehicle repair-call userid=%d veh=%d hp=%.0f/%.0f station=%d pos=(%.0f %.0f %.0f)\n",
					userId, v.vehicleIndex, health, v.maxHealth,
					station != nullptr ? engine->IndexOfEdict(station) : -1, origin.x, origin.y, origin.z);
			}
		}
		if (arrived && s_repairRequest[team].vehicle == v.vehicleIndex)
		{
			s_repairRequest[team].until = (std::max)(s_repairRequest[team].until, now + 5.0f);
			s_repairRequest[team].armorDamaged = worstSide >= 0 && worstArmor < 0.95f;
			s_repairRequest[team].side = worstSide;
		}
	}

	if (lawlietDrive)
	{
		v.fightMove = false;
		ram = false;
		driveTo = lawlietHold ? origin : lawlietDriveTo;
		arrive = lawlietArrive;
		throttle = lawlietThrottle;
	}

	if (v.commanderCV && target == nullptr)
		arrive = v.cvArrive;
	// -------- follow a navmesh route unless the goal is close and in sight
	// A new vehicle stands in the factory bay, inside the factory's box. Planned from there, the
	// building detour ran along the factory side into a gap between crates and a tower instead of
	// out of the bay (human test 15.11., run 20261115-125702, stuck at (1708 9688)). First drive
	// straight ahead (the bay faces out) until clear of every building box, then plan.
	if (!v.exitChecked && !v.commanderCV)
	{
		v.exitChecked = true;
		const float pad = v.width * 0.5f + 112.0f;
		if (RCBotNavMesh::PointNearBuilding(origin, 0.0f))
		{
			const Vector dir = YawDir(heading);
			for (float d = 200.0f; d <= 2200.0f; d += 50.0f)
			{
				const Vector p = origin + dir * d;
				if (!DriveLineClear(origin, p, v.width * 0.5f))
					break;
				v.exitPoint = p;
				if (!RCBotNavMesh::PointNearBuilding(p, pad))
					break;
			}
			if (Dist2D(origin, v.exitPoint) >= 200.0f)
			{
				v.exitUntil = now + 6.0f + Dist2D(origin, v.exitPoint) / 150.0f;
				RCBOT_DIAG("[RCBOT2] vehicle factory-exit start userid=%d veh=%d from=(%.0f %.0f) to=(%.0f %.0f) dist=%.0f clear=%d\n",
					userId, v.vehicleIndex, origin.x, origin.y, v.exitPoint.x, v.exitPoint.y, Dist2D(origin, v.exitPoint),
					RCBotNavMesh::PointNearBuilding(v.exitPoint, pad) ? 0 : 1);
			}
		}
	}
	if (v.exitUntil > 0.0f)
	{
		const bool clear = !RCBotNavMesh::PointNearBuilding(origin, v.width * 0.5f + 112.0f);
		if (clear || now >= v.exitUntil)
		{
			RCBOT_DIAG("[RCBOT2] vehicle factory-exit end userid=%d reason=%s dist=%.0f pos=(%.0f %.0f)\n", userId,
				clear ? "reached" : "timeout", Dist2D(origin, v.exitPoint), origin.x, origin.y);
			v.exitUntil = 0.0f;
			v.nextReplan = 0.0f;
		}
		else
		{
			// Normal route arrival and steering must wait until the vehicle clears the factory.
			const float look = (std::max)(160.0f, speed * 0.6f);
			const bool safe = SafeHeading(origin, heading, look) &&
				DriveLineClear(origin, origin + YawDir(heading) * look, v.width * 0.5f);
			v.forward = safe && speed < 400.0f ? kMaxMove : 0.0f;
			v.side = 0.0f;
			v.buttons = safe ? 0 : 0x08000000;
			v.view = QAngle(0, heading, 0);
			if (now >= v.nextLog)
			{
				v.nextLog = now + 2.0f;
				RCBOT_DIAG("[RCBOT2] vehicle factory-exit drive userid=%d pos=(%.0f %.0f %.0f) heading=%.0f speed=%.0f fwd=%.0f safe=%d\n",
					userId, origin.x, origin.y, origin.z, heading, speed, v.forward, safe ? 1 : 0);
				if (rcbot_empires_debug_log.GetBool())
					LogFactoryExitHull(userId, m_pEdict, vehicle, origin, heading, look);
			}
			return;
		}
	}
	// Run the same throttle and steering sequence in both modes. Use the real
	// driver input path and stop sampling before handing control back to the bot.
	if (turnTest)
	{
		v.view = QAngle(0, heading, 0);
		// A debug vehicle drops from above the buyer. Let the suspension settle
		// before accelerating so mid-air input cannot enter either sample.
		if (now - v.stageSince < 1.0f || (v.turnTestStart == 0.0f &&
			now - v.stageSince <= 5.0f && std::fabs(v.velocity.z) > 40.0f))
			return;
		const Vector ahead = origin + YawDir(heading) * 420.0f;
		const bool slow = v.turnTestStart == 0.0f && now - v.stageSince > 5.0f;
		if (slow || !SafeHeading(origin, heading, 420.0f) ||
			!DriveLineClear(origin, ahead, v.width * 0.5f) || target != nullptr)
		{
			v.turnTestStart = -1.0f;
			RCBOT_DIAG("[RCBOT2] vehicle turn-test abort userid=%d chassis=%d reason=%s\n",
				userId, chassis, slow ? "entry-speed-timeout" : "obstruction-or-contact");
			return;
		}
		if (v.turnTestStart == 0.0f && forwardSpeed >= (turnTestMode >= 3 ? 500.0f : 350.0f))
		{
			v.turnTestStart = now;
			v.turnTestHeading = heading;
			v.turnTestMode = turnTestMode;
		}
		const float elapsed = v.turnTestStart > 0.0f ? now - v.turnTestStart : -1.0f;
		const bool straight = elapsed >= 0.0f && v.turnTestMode >= 3;
		const bool stopped = straight && forwardSpeed <= 80.0f;
		const bool finished = elapsed >= 2.0f || stopped;
		v.forward = kMaxMove;
		if (straight)
			v.forward = v.turnTestMode == 4 && !finished ? -kMaxMove * 0.6f : 0.0f;
		v.side = !straight && elapsed >= 0.0f && elapsed < 0.5f ? kMaxMove : 0.0f;
		v.buttons = elapsed >= 0.0f && elapsed < 0.2f && v.turnTestMode == 2 ? 0x08000000 : 0;
		if (elapsed >= 0.0f && (now >= v.turnTestNextSample || finished))
		{
			v.turnTestNextSample = now + 0.1f;
			RCBOT_DIAG("[RCBOT2] vehicle turn-test %s userid=%d chassis=%d mode=%d t=%.3f "
				"pos=(%.1f %.1f %.1f) heading=%.1f yaw=%.1f speed=%.1f fwd=%.1f lateral=%.1f\n",
				finished ? "end" : "sample", userId, chassis, v.turnTestMode, elapsed,
				origin.x, origin.y, origin.z, heading, NormalizeYaw(heading - v.turnTestHeading),
				speed, forwardSpeed, DotProduct(v.velocity, YawDir(heading + 90.0f)));
		}
		if (finished)
			v.turnTestStart = -1.0f;
		return;
	}
	const bool closeManeuver = (v.billNye && targetIsVehicle) ||
		(target != nullptr && Dist2D(origin, driveTo) < 900.0f &&
			DriveLineClear(origin, driveTo, v.width * 0.5f));
	bool onRoute = false;
	bool clipBlockedRoute = false;
	Vector maneuverFrom = origin; // start of the route line the local planner scores against
	Vector maneuverNext = origin; // route point after the lookahead point
	float curveSpeed = 0.0f;
	const bool regulateCurveSpeed = target == nullptr && !v.padInside &&
		!v.billNye && rcbot_empires_vehicle_curve_speed.GetBool();
	if (!closeManeuver && Dist2D(origin, driveTo) > 300.0f)
	{
		const Vector routeTarget = driveTo;
		const edict_t* drivenCV = v.commanderCV ? vehicle : nullptr;
		const edict_t* sightVehicle = v.padInside ? nullptr : vehicle;
		if (now >= v.nextReplan || Dist2D(v.routeGoal, driveTo) > 500.0f)
		{
			const bool sameGoal = Dist2D(v.routeGoal, driveTo) <= 500.0f;
			v.nextManeuver = 0.0f;
			v.nextReplan = now + 4.0f;
			v.routeGoal = driveTo;
			RCBotNavMesh::Path path;
			const RCBotNavMesh::PathResult result =
				RCBotNavMesh::BuildVehiclePath(origin, driveTo, path, v.width, true, v.waterDepth, vehicle,
					v.commanderCV ? rcbot_empires_cv_max_climb.GetFloat() : 0.0f);
			const bool ok = result == RCBotNavMesh::PathResult::SUCCESS && path.points.size() >= 2;
			if (static_cast<int>(result) != v.routeResult || (ok && v.route.empty()) || path.partial != v.routePartial)
				RCBOT_DIAG("[RCBOT2] vehicle route userid=%d result=%s%s points=%zu length=%.0f from=(%.0f %.0f %.0f) "
					"to=(%.0f %.0f %.0f) width=%.0f end_gap=%.0f\n", userId, RCBotNavMesh::PathResultName(result),
					path.partial ? "-partial" : "", path.points.size(), path.length, origin.x, origin.y, origin.z,
					driveTo.x, driveTo.y, driveTo.z, v.width,
					path.points.empty() ? -1.0f : Dist2D(path.points.back(), driveTo));
			// No path from here (slid off the route on a slope): without a route the driver steered
			// straight at the goal, over the shore into the sea, and the NF CV drowned (Palmbay
			// 20261125-135810, game over at 7:14). Keep driving the last route to the same goal
			// until a new plan succeeds; it leads back onto mesh the planner knows.
			if (!ok && sameGoal && target == nullptr && v.route.size() >= 2 &&
				rcbot_empires_vehicle_keep_route.GetBool())
			{
				if (static_cast<int>(result) != v.routeResult)
					RCBOT_DIAG("[RCBOT2] vehicle route-keep userid=%d result=%s idx=%zu/%zu pos=(%.0f %.0f %.0f)\n",
						userId, RCBotNavMesh::PathResultName(result), v.routeIndex, v.route.size(),
						origin.x, origin.y, origin.z);
				v.routeResult = static_cast<int>(result);
				v.nextReplan = now + 2.0f;
			}
			else
			{
				v.routePartial = ok && path.partial;
				v.routeResult = static_cast<int>(result);
				v.routeFailed = !ok;
				v.route = ok ? path.points : std::vector<Vector>();
				v.routePrecise = ok ? path.precisePoints : std::vector<Vector>();
				v.preciseIndex = static_cast<std::size_t>(-1);
				v.routeAreas = ok ? path.areaIds : std::vector<unsigned int>();
				v.routeIndex = 1;
			}
		}
		clipBlockedRoute = v.routeResult == static_cast<int>(RCBotNavMesh::PathResult::VEHICLE_CLIP_BLOCKED);
		// 9.8: without a path the CV drove straight at its goal from the Palmbay shore lowland, up a
		// >40 deg bank, slid back and drowned (run 20261125-171744, game over at 10:53). End the drive;
		// the commander drops the site and picks another one.
		if (v.route.empty() && v.routeFailed && v.commanderCV && !v.cvHunt && !v.cvArrived && target == nullptr &&
			rcbot_empires_cv_no_path_stop.GetBool())
		{
			v.cvArrived = true;
			v.cvEnd = "no-path";
			RCBOT_DIAG("[RCBOT2] stall-escalate who=cv userid=%d stage=3 reason=no-path pos=(%.0f %.0f %.0f) "
				"goal=(%.0f %.0f %.0f)\n", userId, origin.x, origin.y, origin.z, driveTo.x, driveTo.y, driveTo.z);
		}
		if (!v.route.empty())
		{
			// Tight-lane points (rcbot_empires_vehicle_wall_clearance): reached within 90, no lookahead past them;
			// after 8 s on one of them the normal rules apply again so a missed point cannot trap the driver.
			auto isPrecise = [&](std::size_t index) {
				if (index >= v.route.size())
					return false;
				for (const Vector& point : v.routePrecise)
					if (Dist2D(point, v.route[index]) < 2.0f)
						return true;
				return false;
			};
			// Only the current target ages; a point ahead keeps its full time.
			auto precisePoint = [&](std::size_t index) {
				if (!isPrecise(index))
					return false;
				if (index != v.routeIndex)
					return true;
				if (v.preciseIndex != index)
				{
					v.preciseIndex = index;
					v.preciseSince = now;
				}
				return now - v.preciseSince < 8.0f;
			};
			// Advance past points we reached or passed.
			while (v.routeIndex + 1 < v.route.size())
			{
				const Vector& p = v.route[v.routeIndex];
				const Vector& q = v.route[v.routeIndex + 1];
				Vector seg = q - p;
				seg.z = 0.0f;
				Vector toP = p - origin;
				toP.z = 0.0f;
				const float distToP = Dist2D(origin, p);
				const bool reached = precisePoint(v.routeIndex) ? distToP < 90.0f :
					(distToP < (std::max)(160.0f, v.width) ||
					 (DotProduct(toP, seg) < 0.0f && toP.Length() < 450.0f));
				trace_t blocked;
				// Non-CV vehicles also check line-of-sight before advancing early (distToP >= 120),
				// so they do not skip the gate-entry waypoint and clip Crossroads gate center pillars.
				const bool lineClear = drivenCV != nullptr
					? DriveLineClear(origin, q, v.width * 0.5f, drivenCV, &blocked, true)
					: (distToP < 120.0f || DriveLineClear(origin, q, v.width * 0.5f, sightVehicle, &blocked, false));
				if (reached && lineClear)
					++v.routeIndex;
				else
				{
					if (reached && now >= v.nextAdvanceLog)
					{
						v.nextAdvanceLog = now + 1.0f;
						const edict_t* hitEdict = blocked.m_pEnt != nullptr ? servergameents->BaseEntityToEdict(blocked.m_pEnt) : nullptr;
						RCBOT_DIAG("[RCBOT2] vehicle advance-blocked userid=%d idx=%zu/%zu pos=(%.0f %.0f %.0f) p=(%.0f %.0f) q=(%.0f %.0f) hit=%s frac=%.2f at=(%.0f %.0f %.0f)\n",
							userId, v.routeIndex, v.route.size(), origin.x, origin.y, origin.z, p.x, p.y, q.x, q.y,
							hitEdict != nullptr ? hitEdict->GetClassName() : "world", blocked.fraction, blocked.endpos.x,
							blocked.endpos.y, blocked.endpos.z);
					}
					break;
				}
			}
			// Off the route: plan again soon.
			if (Dist2D(origin, v.route[v.routeIndex]) > 1200.0f)
				v.nextReplan = (std::min)(v.nextReplan, now + 0.5f);
			// Scan ahead along the route (up to ~1100 HU, cached per routeIndex) for the next tight passage
			// using world + static props + solid buildings/gates, so both lookahead and context steering
			// prepare before entering the bend in front of a gate.
			if (rcbot_empires_vehicle_context_steer.GetBool() && enginetrace != nullptr &&
				v.routeIndex < v.route.size() &&
				(v.ctxNarrowIndex != v.routeIndex || v.ctxNarrowRouteSize != v.route.size() ||
					Dist2D(v.ctxNarrowAt, v.route[v.routeIndex]) > 1.0f))
			{
				v.ctxNarrowIndex = v.routeIndex;
				v.ctxNarrowRouteSize = v.route.size();
				v.ctxNarrowAt = v.route[v.routeIndex];
				v.ctxNarrow = false;
				v.ctxGateRouteIndex = static_cast<std::size_t>(-1);
				CVehicleSightFilter gateFilter(vehicle);
				const float halfWidth = v.width * 0.5f;
				float scanArc = 0.0f;
				for (std::size_t k = v.routeIndex; k + 1 < v.route.size() && k < v.routeIndex + 8; ++k)
				{
					if (k > v.routeIndex)
					{
						scanArc += Dist2D(v.route[k - 1], v.route[k]);
						if (scanArc > 1100.0f)
							break;
					}
					const Vector& cand = v.route[k];
					// Measure the passage axis over at least one vehicle width so a short navmesh edge
					// cannot tilt the gate axis.
					std::size_t axisEnd = k + 1;
					while (axisEnd + 1 < v.route.size() && axisEnd < k + 4 &&
						Dist2D(cand, v.route[axisEnd]) < v.width)
						++axisEnd;
					const float candAxis = YawTo(cand, v.route[axisEnd]);
					const Vector dir = YawDir(candAxis);
					const Vector side(-dir.y, dir.x, 0.0f);
					float narrowest = 1.0e9f;
					for (const float ahead : {0.0f, 200.0f})
					{
						const Vector at = cand + dir * ahead + Vector(0, 0, 40);
						float free = 0.0f;
						for (const float s : {-1.0f, 1.0f})
						{
							trace_t hit;
							Ray_t ray;
							ray.Init(at, at + side * (s * (halfWidth + 300.0f)));
							enginetrace->TraceRay(ray, MASK_PLAYERSOLID, &gateFilter, &hit);
							free += CtxWallFraction(hit) * (halfWidth + 300.0f);
						}
						narrowest = (std::min)(narrowest, free);
					}
					if (narrowest < v.width + 400.0f)
					{
						v.ctxNarrow = true;
						v.ctxGateRouteIndex = k;
						v.ctxGatePoint = cand;
						v.ctxAxis = candAxis;
						v.ctxGateFree = narrowest - v.width;
						break;
					}
				}
			}
			// Look ahead along the route as far as the body has a clear line; do not cut past an
			// upcoming tight passage while still approaching it.
			const float look = (std::max)(350.0f, (std::min)(1400.0f, 300.0f + speed * 0.7f));
			std::size_t pick = v.routeIndex;
			float along = Dist2D(origin, v.route[v.routeIndex]);
			for (std::size_t k = v.routeIndex + 1; k < v.route.size(); ++k)
			{
				if (!v.routePrecise.empty() && precisePoint(k - 1))
					break;
				if (v.ctxNarrow && k > v.ctxGateRouteIndex)
					break;
				along += Dist2D(v.route[k - 1], v.route[k]);
				if (along > look || std::fabs(v.route[k].z - origin.z) > 200.0f ||
					RCBotNavMesh::VehicleClipBlocks(origin, v.route[k], v.width) ||
					!DriveLineClear(origin, v.route[k], v.width * 0.5f, sightVehicle))
					break;
				pick = k;
			}
			driveTo = v.route[pick];
			maneuverFrom = pick > 0 ? v.route[pick - 1] : origin;
			maneuverNext = pick + 1 < v.route.size() ? v.route[pick + 1] : v.route[pick];
			onRoute = true;
			// The pad is not on the navmesh: a route to it ends at the mesh edge in front of the
			// ramp. Braking there (arrive) left a tank 322 from the centre, just outside the
			// direct-drive range (dev_check 20261124-112956). Past that end drive at the pad.
			if (v.padInside && pick + 1 == v.route.size() && Dist2D(origin, driveTo) < 300.0f &&
				Dist2D(driveTo, routeTarget) > arrive)
				driveTo = routeTarget;
			if (pick + 1 < v.route.size() || Dist2D(origin, driveTo) > 300.0f || v.routePartial)
				arrive = 0.0f;
			// Preview curvature along the actual upcoming route (arc-length resampled, all chassis).
			if (regulateCurveSpeed)
				curveSpeed = RouteCurveSpeed(origin, v.route, v.routeIndex, chassis);
			// Slow down before a sharp corner.
			else if (pick + 1 < v.route.size())
			{
				const float turn = std::fabs(NormalizeYaw(YawTo(v.route[pick], v.route[pick + 1]) -
					YawTo(origin, v.route[pick])));
				if (turn > 50.0f && Dist2D(origin, v.route[pick]) < 200.0f + speed * 0.8f)
					throttle = (std::min)(throttle, speed > 450.0f ? 0.0f : 0.6f);
			}
		}
	}
	else
	{
		v.route.clear();
		v.routeAreas.clear();
		v.nextReplan = 0.0f;
	}

	// Committed detour (local maneuvers): beside the obstacle, then past it.
	if (v.detourActive)
	{
		Vector pastExit = origin - v.detourExit;
		pastExit.z = 0.0f;
		const bool keep = rcbot_empires_vehicle_maneuvers.GetBool() && onRoute && target == nullptr &&
			!v.retreating && now < v.detourUntil;
		const bool passed = DotProduct(pastExit, v.detourDir) > 0.0f;
		if (!keep || passed)
		{
			RCBOT_DIAG("[RCBOT2] vehicle detour end userid=%d entity=%d reason=%s pos=(%.0f %.0f %.0f) speed=%.0f\n",
				userId, v.detourEntity, passed ? "passed" : (now >= v.detourUntil ? "timeout" : "off"),
				origin.x, origin.y, origin.z, speed);
			v.detourActive = false;
			v.maneuverValid = false;
			v.nextManeuver = 0.0f;
			v.nextReplan = (std::min)(v.nextReplan, now + 0.2f);
		}
		else
		{
			Vector pastPoint = origin - v.detourPoint;
			pastPoint.z = 0.0f;
			if (v.detourStage == 0 && DotProduct(pastPoint, v.detourDir) > -60.0f)
				v.detourStage = 1;
			driveTo = v.detourStage == 0 ? v.detourPoint : v.detourExit;
			maneuverFrom = origin;
			maneuverNext = v.detourExit;
			// Hold the speed the detour was checked for: full throttle through
			// the turn widened the arc into the barrier corner (NF run
			// 20261119-145653, stuck 110 units from it at err -64).
			if (forwardSpeed > 560.0f)
				throttle = 0.0f;
			else if (forwardSpeed > 520.0f)
				throttle = (std::min)(throttle, 0.3f);
		}
	}

	// -------- steering with edge avoidance
	const float goalDistance = Dist2D(origin, driveTo);
	// A human driver notices when the CV makes no headway. Rocking on a slope stays above the
	// speed < 40 stuck check (Palmbay run 20261120-205131: CV slid off the plateau 315 from its goal
	// and rocked ~3.5 game minutes before the first stuck-reverse; the economy then built the factory
	// remotely at once). Progress is a shorter way to cvGoal or a shorter remaining route (long detours).
	if (v.commanderCV && !v.cvArrived && !v.cvHunt)
	{
		const float toGoal = Dist2D(origin, v.cvGoal);
		float remaining = -1.0f;
		if (onRoute && v.routeIndex < v.route.size())
		{
			remaining = Dist2D(origin, v.route[v.routeIndex]);
			for (std::size_t k = v.routeIndex + 1; k < v.route.size(); ++k)
				remaining += Dist2D(v.route[k - 1], v.route[k]);
		}
		if (v.cvProgressAt == 0.0f || (remaining >= 0.0f && remaining > v.cvBestRoute + 2000.0f))
		{
			// Drive start or a new, longer route: measure from here.
			v.cvProgressAt = now;
			v.cvBestGoal = toGoal;
			v.cvBestRoute = remaining >= 0.0f ? remaining : 1.0e9f;
		}
		else if (toGoal < v.cvBestGoal - 200.0f || (remaining >= 0.0f && remaining < v.cvBestRoute - 200.0f))
		{
			v.cvProgressAt = now;
			v.cvBestGoal = (std::min)(v.cvBestGoal, toGoal);
			if (remaining >= 0.0f)
				v.cvBestRoute = (std::min)(v.cvBestRoute, remaining);
		}
		else if (now - v.cvProgressAt > 25.0f)
		{
			v.cvArrived = true;
			v.cvEnd = "stalled";
			RCBOT_DIAG("[RCBOT2] stall-escalate who=cv userid=%d stage=1 reason=no-progress pos=(%.0f %.0f %.0f) "
				"to_goal=%.0f best=%.0f route_left=%.0f best_route=%.0f for=%.0f\n", userId, origin.x, origin.y, origin.z,
				toGoal, v.cvBestGoal, remaining, v.cvBestRoute < 1.0e9f ? v.cvBestRoute : -1.0f, now - v.cvProgressAt);
		}
	}
	const float desired = YawTo(origin, driveTo);
	const bool localManeuvers = rcbot_empires_vehicle_maneuvers.GetBool() && onRoute && arrive == 0.0f &&
		(chassis == 2 || chassis == 10) && target == nullptr && !v.retreating && !v.billNye &&
		enginetrace != nullptr && vehicle->GetCollideable() != nullptr;
	// Bill Nye pushing a vehicle must get close to the edge; running over
	// infantry at speed needs to see the edge in time to stop.
	// Short only in contact: a missed ram at 900 u/s went over the map end
	// (run 202056, four Bill Nye falls).
	const bool shortLook = v.billNye && targetIsVehicle && v.ramming && Dist2D(origin, targetAt) < 450.0f;
	const float lookAhead = shortLook ? 200.0f :
		ram ? (std::max)(200.0f, (std::min)(1400.0f, speed * 0.9f)) :
		(std::max)(260.0f, (std::min)(1500.0f, speed * 1.1f + 200.0f));
	const float rearLook = (std::max)(220.0f, (std::min)(600.0f, speed * 0.6f));
	const bool rearSafe = SafeHeading(origin, heading + 180.0f, rearLook);
	static const float kOffsets[] = {0, 15, -15, 30, -30, 50, -50, 75, -75, 100, -100, 135, -135, 180};
	float chosen = desired;
	bool found = false;
	for (const float offset : kOffsets)
	{
		const float yaw = NormalizeYaw(desired + offset);
		if (SafeHeading(origin, yaw, lookAhead))
		{
			chosen = yaw;
			found = true;
			break;
		}
	}
	// Context steering (rcbot_empires_vehicle_context_steer): gate run 20261127-104131, a BE Heavy came in
	// at -150 for a -83 gate, chased the gate point and pressed its nose into the wall beside it for 10 s.
	if (rcbot_empires_vehicle_context_steer.GetBool() && onRoute && found && target == nullptr && !v.billNye &&
		!ram && enginetrace != nullptr && v.routeIndex < v.route.size())
	{
		namespace CS = EmpiresContextSteer;
		CVehicleSightFilter solids(vehicle);
		const float traceLift = VehicleTraceLift(vehicle);
		const Vector eye = origin + Vector(0, 0, traceLift);
		const float halfWidth = v.width * 0.5f;
		auto traceFrac = [&](const Vector& to) {
			trace_t hit;
			Ray_t ray;
			ray.Init(eye, to + Vector(0, 0, traceLift));
			enginetrace->TraceRay(ray, MASK_PLAYERSOLID, &solids, &hit);
			return hit.startsolid ? 0.0f : CtxWallFraction(hit);
		};
		const float reach = (std::max)(300.0f, (std::min)(700.0f, 250.0f + speed * 0.6f));
		// Danger along one yaw: the centre line plus both front corners swept along it (corner start
		// pulled in a little so a hull already touching a wall does not start inside it).
		const Vector hullDir = YawDir(heading);
		const Vector hullLeft(-hullDir.y, hullDir.x, 0.0f);
		auto traceFrom = [&](const Vector& from, const Vector& to) {
			trace_t hit;
			Ray_t ray;
			ray.Init(from + Vector(0, 0, traceLift), to + Vector(0, 0, traceLift));
			enginetrace->TraceRay(ray, MASK_PLAYERSOLID, &solids, &hit);
			return hit.startsolid ? 1.0f : CtxWallFraction(hit); // inside: that corner says nothing
		};
		auto dangerAt = [&](float yaw) {
			const Vector dir = YawDir(yaw);
			float frac = traceFrac(origin + dir * reach);
			for (const float s : {-1.0f, 1.0f})
			{
				const Vector corner = origin + hullDir * 150.0f + hullLeft * (s * (halfWidth - 20.0f));
				frac = (std::min)(frac, traceFrom(corner, corner + dir * reach));
			}
			return 1.0f - frac;
		};
		// Tight passage detected ahead by the cached route scan (above): line up early onto its axis
		// and actively brake if arriving off-axis or off-centre.
		float want = chosen;
		const Vector& gate = v.ctxNarrow ? v.ctxGatePoint : v.route[v.routeIndex];
		const float toGate = Dist2D(origin, gate);
		if (v.ctxNarrow && toGate > 150.0f && toGate < 1200.0f)
		{
			const float entryYaw = CS::EntryAim(origin.x, origin.y, gate.x, gate.y, v.ctxAxis, TurnRadius(chassis, speed));
			// Follow EntryAim immediately when on the final segment before the passage, or earlier when
			// the line onto the passage axis is unobstructed (avoid cutting through blind corners).
			if (v.ctxGateRouteIndex == v.routeIndex || (toGate < 900.0f && dangerAt(entryYaw) < CS::kDesiredSafe))
				want = entryYaw;
			const float passCap = CS::PassageApproachSpeed(origin.x, origin.y, heading, gate.x, gate.y, v.ctxAxis,
				v.ctxGateFree, CS::ProfileForChassis(chassis));
			if (passCap > 0.0f)
				curveSpeed = curveSpeed > 0.0f ? (std::min)(curveSpeed, passCap) : passCap;
			// Fallback throttle moderation if already close and misaligned.
			const float off = std::fabs(NormalizeYaw(want - heading));
			if (off > 25.0f && toGate < 300.0f + speed * 0.8f)
				throttle = (std::min)(throttle, speed > 250.0f ? 0.0f : 0.5f);
		}
		// Only at tight passages: in open ground the route already bends around buildings, and slot
		// scoring against them pushed drivers off it (crossroads A/B 20261127-105737: 55 reversals vs 25).
		const bool atPassage = v.ctxNarrow && toGate < 1200.0f;
		if (!atPassage)
		{
			v.ctxSlot = -2;
			v.ctxHasPrev = false;
		}
		else if (now >= v.ctxNextEval)
		{
			v.ctxNextEval = now + 0.1f;
			const float wantDanger = dangerAt(want);
			CS::Result r;
			if (wantDanger < CS::kDesiredSafe)
				r.yaw = want;
			else
			{
				float danger[CS::kSlots];
				for (int i = 0; i < CS::kSlots; ++i)
					danger[i] = std::fabs(CS::SlotOffset(i)) <= CS::kForwardLimit ? dangerAt(CS::SlotYaw(heading, i)) : 1.0f;
				r = CS::Choose(heading, want, danger, wantDanger, v.ctxHasPrev, v.ctxPrevYaw);
			}
			v.ctxYaw = r.blocked ? chosen : r.yaw;
			v.ctxSlot = r.blocked ? -2 : r.slot;
			if (now >= v.ctxNextLog)
			{
				v.ctxNextLog = now + 0.25f;
				RCBOT_DIAG("[RCBOT2] vehicle ctx-steer userid=%d veh=%d pos=(%.0f %.0f) heading=%.0f route_yaw=%.0f want=%.0f "
					"yaw=%.0f slot=%d danger=%.2f narrow=%d gate_free=%.0f axis=%.0f to_gate=%.0f speed=%.0f\n", userId, v.vehicleIndex,
					origin.x, origin.y, heading, chosen, want, v.ctxYaw, v.ctxSlot, wantDanger, v.ctxNarrow ? 1 : 0,
					v.ctxGateFree, v.ctxAxis, toGate, speed);
			}
		}
		if (v.ctxSlot != -2 && SafeHeading(origin, v.ctxYaw, lookAhead))
		{
			chosen = v.ctxYaw;
			v.ctxPrevYaw = chosen;
			v.ctxHasPrev = true;
		}
	}
	else
		v.ctxHasPrev = false;
	const bool headingSafe = SafeHeading(origin, heading, lookAhead);

	// Bail out before an unavoidable fall (user: falling off is fine, but leave the vehicle first;
	// the game allows exit up to 100 mph = 1760 u/s, CEmpTank::CanExitVehicle, not before 1 s after
	// boarding). Not for the commander's CV and not while Bill Nye pushes in contact (!shortLook; falling together is wanted).
	// A: braking distance no longer reaches the edge and turning left/right is no way out either.
	// B: last moment, no ground under the nose and under the vehicle itself.
	// The deceleration (kBailDecel) is a guess until measured, hence the log line.
	if (v.stage == EmpiresVehicleStage::DRIVING && !v.commanderCV && !IsCommanderChassis(chassis) &&
		now - v.stageSince > 1.0f && speed > 60.0f && !shortLook)
	{
		constexpr float kBailDecel = 600.0f;
		const float velYaw = YawTo(Vector(0.0f, 0.0f, 0.0f), v.velocity);
		const Vector velDir = YawDir(velYaw);
		const float drop = DropDistance(origin, velYaw, 1500.0f);
		const float brake = speed * speed / (2.0f * kBailDecel) + speed * 0.15f;
		const char* bailReason = nullptr;
		// DropDistance returns 2*range when no drop was found (runs 125334: 3 false alarms at 1900 u/s).
		// Run 125647: 4 of 8 at 226-257 u/s with 150 to the edge would have stopped in time.
		if (speed > 350.0f && drop <= 1500.0f && drop < brake &&
			!SafeHeading(origin, velYaw + 45.0f, (std::max)(200.0f, drop)) &&
			!SafeHeading(origin, velYaw - 45.0f, (std::max)(200.0f, drop)))
			bailReason = "A";
		else if (!GroundAt(origin + velDir * (v.width * 0.5f + 40.0f), origin.z) && !GroundAt(origin, origin.z))
			bailReason = "B";
		if (bailReason != nullptr)
		{
			RCBOT_DIAG("[RCBOT2] vehicle bail-out userid=%d veh=%d chassis=%d pos=(%.0f %.0f %.0f) speed=%.0f drop=%.0f brake=%.0f reason=%s\n",
				userId, v.vehicleIndex, chassis, origin.x, origin.y, origin.z, speed, drop, brake, bailReason);
			v.nextAction = now;
			setStage(EmpiresVehicleStage::GIVE_UP_EXIT, 3.0f);
			return;
		}
	}
	// Circling into an edge: turn the other way round the target.
	if (v.fightMove && (!found || std::fabs(NormalizeYaw(chosen - desired)) > 60.0f))
		v.fightSwitch = (std::min)(v.fightSwitch, now);
	// Fighting with a weakened side: present the strongest side to the target.
	// Sides relative to the hull: 0 front, 1 rear, 2 left (+90), 3 right (-90).
	const bool targetArmedThreat = target != nullptr && (
		targetIsVehicle ||
		engine->IndexOfEdict(target) <= gpGlobals->maxClients ||
		(target->GetClassName() != nullptr &&
			(std::strstr(target->GetClassName(), "mgturret") != nullptr ||
			 std::strstr(target->GetClassName(), "mlturret") != nullptr)));
	const bool inDirectFight = !onRoute && (now - v.fightSight < 2.0f);
	bool facing = false;
	if (armed && !v.retreating && !lawlietDrive && targetArmedThreat && inDirectFight &&
		worstSide >= 0 && bestSide >= 0 && worstArmor < 0.6f && bestSide != worstSide &&
		Dist2D(origin, targetAt) < 2500.0f)
	{
		static const float kSideOffset[4] = {0.0f, 180.0f, 90.0f, -90.0f};
		const float faceYaw = NormalizeYaw(YawTo(origin, targetAt) - kSideOffset[bestSide]);
		if (SafeHeading(origin, faceYaw, 260.0f))
		{
			chosen = faceYaw;
			facing = true;
		}
	}
	const float err = NormalizeYaw(chosen - heading); // + = turn left

	float forward = kMaxMove * throttle;
	const float routeSteer = (std::max)(-1.0f, (std::min)(1.0f, -err / 30.0f));
	// Diagnostic for tight passages (gates): with walls close beside the hull log the heading against
	// the route segment and the free space to both sides (4 Hz).
	if (onRoute && v.routeIndex > 0 && v.routeIndex < v.route.size() && now >= v.nextTightLog && enginetrace != nullptr)
	{
		v.nextTightLog = now + 0.25f;
		const Vector dir = YawDir(heading);
		const Vector left(-dir.y, dir.x, 0.0f);
		CTraceFilterWorldOnly worldOnly;
		auto clearance = [&](const Vector& from, const Vector& way) {
			trace_t hit;
			Ray_t ray;
			ray.Init(from + Vector(0, 0, 48), from + Vector(0, 0, 48) + way * 600.0f);
			enginetrace->TraceRay(ray, MASK_PLAYERSOLID, &worldOnly, &hit);
			return hit.fraction * 600.0f - v.width * 0.5f;
		};
		const Vector nose = origin + dir * 180.0f;
		const float cl = clearance(origin, left), cr = clearance(origin, -left);
		const float nl = clearance(nose, left), nr = clearance(nose, -left);
		if ((std::min)((std::min)(cl, cr), (std::min)(nl, nr)) < 250.0f)
		{
			const float axis = YawTo(v.route[v.routeIndex - 1], v.route[v.routeIndex]);
			RCBOT_DIAG("[RCBOT2] vehicle tight userid=%d veh=%d pos=(%.0f %.0f %.0f) heading=%.0f axis=%.0f dev=%.0f "
				"idx=%zu/%zu to=(%.0f %.0f) clr_l=%.0f clr_r=%.0f nose_l=%.0f nose_r=%.0f ahead=%.0f err=%.0f steer=%.2f fwd=%.2f speed=%.0f\n",
				userId, v.vehicleIndex, origin.x, origin.y, origin.z, heading, axis, NormalizeYaw(heading - axis),
				v.routeIndex, v.route.size(), driveTo.x, driveTo.y, cl, cr, nl, nr, clearance(origin, dir) + v.width * 0.5f,
				err, routeSteer, throttle, speed);
		}
	}
	float side = routeSteer * kMaxMove;
	if (!localManeuvers)
	{
		v.nextManeuver = 0.0f;
		v.maneuverRecoverUntil = 0.0f;
		v.maneuverSpeed = 0.0f;
		v.maneuverSteer = 0.0f;
		v.maneuverValid = false;
	}
	else if (now >= v.nextManeuver)
	{
		// Shared budget: at most N evaluations per server tick for all vehicles.
		// The others keep their last choice and try again next tick.
		if (gpGlobals->tickcount != g_maneuverTick)
		{
			g_maneuverTick = gpGlobals->tickcount;
			g_maneuverTickCalls = 0;
		}
		if (g_maneuverTickCalls >= (std::max)(1, rcbot_empires_vehicle_maneuver_budget.GetInt()))
			++g_maneuverPerf.deferred;
		else
		{
			++g_maneuverTickCalls;
			int traces = 0;
			const auto started = std::chrono::steady_clock::now();
			for (int k = 0; k < kManeuverPassableSlots; ++k)
				if (v.maneuverPassable[k] >= 0 && now >= v.maneuverPassableUntil[k])
					v.maneuverPassable[k] = -1;
			const VehicleManeuver maneuver = ChooseVehicleManeuver(m_pEdict, vehicle, chassis, origin, heading,
				speed, forwardSpeed, driveTo, maneuverNext, maneuverFrom, routeSteer, v.maneuverSpeed,
				v.maneuverValid ? v.maneuverSteer : routeSteer, now < v.maneuverRecoverUntil, v.maneuverPassable, traces);
			if (maneuver.passableCandidate > 0)
			{
				int slot = 0;
				for (int k = 1; k < kManeuverPassableSlots; ++k)
					if (v.maneuverPassableUntil[k] < v.maneuverPassableUntil[slot])
						slot = k;
				v.maneuverPassable[slot] = maneuver.passableCandidate;
				v.maneuverPassableUntil[slot] = now + 20.0f;
				const edict_t* passed = engine->PEntityOfEntIndex(maneuver.passableCandidate);
				RCBOT_DIAG("[RCBOT2] vehicle maneuver-passable userid=%d entity=%d class=%s group=%d speed=%.0f pos=(%.0f %.0f %.0f)\n",
					userId, maneuver.passableCandidate,
					passed != nullptr && !passed->IsFree() && passed->GetClassName() != nullptr ? passed->GetClassName() : "unknown",
					maneuver.blockerGroup, forwardSpeed, origin.x, origin.y, origin.z);
			}
			const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
			++g_maneuverPerf.calls;
			g_maneuverPerf.traces += traces;
			g_maneuverPerf.seconds += seconds;
			g_maneuverPerf.worst = (std::max)(g_maneuverPerf.worst, seconds);
			v.nextManeuver = now + 0.15f;
			v.maneuverSteer = maneuver.steer;
			v.maneuverSpeed = maneuver.speed;
			v.maneuverBlocker = maneuver.blocker;
			v.maneuverValid = maneuver.valid;
			// Only within acting range: a roadside prop 950 units ahead caused a
			// needless 429-unit swerve that timed out (NF VF run 20261119-143854);
			// 500 units started a BE AFV too late (run 20261119-1448, contact).
			if (!v.detourActive && maneuver.blocker > 0 && maneuver.passableCandidate < 0 &&
				maneuver.blockDistance > 40.0f && maneuver.blockDistance <= (std::max)(650.0f, speed * 1.1f))
			{
				Vector point, exitPoint, dir;
				float offset = 0.0f;
				int detourTraces = 0;
				if (PlanVehicleDetour(m_pEdict, vehicle, origin, driveTo, maneuver.blocker, v.maneuverPassable,
					point, exitPoint, dir, offset, detourTraces))
				{
					v.detourActive = true;
					v.detourStage = 0;
					v.detourEntity = maneuver.blocker;
					v.detourPoint = point;
					v.detourExit = exitPoint;
					v.detourDir = dir;
					v.detourUntil = now + 3.0f + Dist2D(origin, exitPoint) / 250.0f;
					v.nextManeuver = 0.0f; // check the detour path on the next tick
					const edict_t* around = engine->PEntityOfEntIndex(maneuver.blocker);
					RCBOT_DIAG("[RCBOT2] vehicle detour start userid=%d entity=%d class=%s side=%s offset=%.0f "
						"point=(%.0f %.0f) exit=(%.0f %.0f) pos=(%.0f %.0f %.0f) speed=%.0f dist=%.0f traces=%d\n",
						userId, maneuver.blocker,
						around != nullptr && !around->IsFree() && around->GetClassName() != nullptr ? around->GetClassName() : "unknown",
						offset > 0.0f ? "left" : "right", offset, point.x, point.y, exitPoint.x, exitPoint.y,
						origin.x, origin.y, origin.z, speed, maneuver.blockDistance, detourTraces);
				}
				g_maneuverPerf.traces += detourTraces;
			}
			if (maneuver.blocker < 0)
				v.nextManeuverBlockLog = 0.0f; // log the first block after a clear stretch at once
			else if (now >= v.nextManeuverBlockLog)
			{
				v.nextManeuverBlockLog = now + 1.0f;
				const edict_t* blocker = maneuver.blocker > 0 ? engine->PEntityOfEntIndex(maneuver.blocker) : nullptr;
				const char* blockerClass = blocker != nullptr && !blocker->IsFree() && blocker->GetClassName() != nullptr ?
					blocker->GetClassName() : (maneuver.blocker == 0 ? "world-or-static" : "unknown");
				RCBOT_DIAG("[RCBOT2] vehicle maneuver-block userid=%d chassis=%d pos=(%.0f %.0f %.0f) speed=%.0f "
					"blocker=%d class=%s group=%d solid=%d contents=0x%x surface=%s dist=%.0f normal_z=%.2f at=(%.0f %.0f %.0f) chosen=%d brake=%d steer=%.2f "
					"maneuver_speed=%.0f hull_z=%.0f..%.0f tick=%d\n", userId, chassis, origin.x, origin.y, origin.z,
					speed, maneuver.blocker, blockerClass, maneuver.blockerGroup, maneuver.blockerSolid,
					maneuver.blockContents, maneuver.blockSurface, maneuver.blockDistance, maneuver.blockNormalZ,
					maneuver.blockAt.x, maneuver.blockAt.y, maneuver.blockAt.z, maneuver.valid ? 1 : 0, maneuver.braking ? 1 : 0,
					maneuver.steer, maneuver.speed, vehicle->GetCollideable()->OBBMins().z,
					vehicle->GetCollideable()->OBBMaxs().z, gpGlobals->tickcount);
			}
		}
		if (now >= g_maneuverPerf.logAt)
		{
			if (g_maneuverPerf.calls > 0)
				RCBOT_DIAG("[RCBOT2] vehicle maneuver-perf calls=%d deferred=%d traces=%d avg_traces=%.1f "
					"avg_us=%.0f max_us=%.0f total_ms=%.2f window=%.1f\n", g_maneuverPerf.calls,
					g_maneuverPerf.deferred, g_maneuverPerf.traces,
					static_cast<double>(g_maneuverPerf.traces) / g_maneuverPerf.calls,
					g_maneuverPerf.seconds * 1.0e6 / g_maneuverPerf.calls, g_maneuverPerf.worst * 1.0e6,
					g_maneuverPerf.seconds * 1.0e3, g_maneuverPerf.logAt > 0.0f ? now - g_maneuverPerf.logAt + 10.0f : 0.0f);
			g_maneuverPerf = ManeuverPerf();
			g_maneuverPerf.logAt = now + 10.0f;
		}
	}
	const bool applyManeuver = localManeuvers && v.maneuverValid && now >= v.reverseUntil;
	if (std::fabs(err) > 70.0f)
		forward = kMaxMove * 0.4f;
	// Too fast for the turn: brake first (run 213421 swung through ±140° at 800 u/s).
	if (onRoute && std::fabs(err) > 40.0f && forwardSpeed > 380.0f)
		forward = -kMaxMove * 0.6f;
	if (curveSpeed > 0.0f && forward >= 0.0f)
	{
		// Reverse input brakes while rolling forward. Leave a dead band before applying it.
		if (forwardSpeed > curveSpeed + 35.0f)
			forward = -kMaxMove * 0.6f;
		else
			forward = (std::min)(forward, kMaxMove * (std::max)(0.0f, (curveSpeed - forwardSpeed) / 120.0f));
	}
	// Tanks turn on the spot (user): for a sharp turn stop and pivot instead
	// of swinging wide with throttle; slow down first when fast. Only chassis
	// with a "Neutral Turn Factor" (RE note: medium, heavy, artillery; not
	// light tank, AFV, APC, jeep), forwardmove exactly 0, speed <= 20 mph.
	const bool pivots = chassis == 3 || chassis == 4 || chassis == 5 || chassis == 11 || chassis == 12 ||
		chassis == 13;
	if (pivots && std::fabs(err) > 50.0f)
	{
		side = err > 0.0f ? -kMaxMove : kMaxMove;
		forward = forwardSpeed > 150.0f ? -kMaxMove * 0.6f : forwardSpeed < -150.0f ? kMaxMove * 0.6f : 0.0f;
	}
	// Vehicles that cannot pivot: from a slow start facing away they crept forward at 0.4
	// throttle (CV: stuck-reverse every hunt start, run 155839), or toward an edge while
	// turning (rainbowroad run 20261112-170711: 18 of 20 falls were vehicles, e.g. 120 u/s
	// with safe=1/0, err 142). Back up with the opposite lock until the nose points within
	// 60 degrees and ahead is ground, at most 3 s.
	const bool mayBack = !applyManeuver && !pivots && !v.billNye && !facing && !v.padInside &&
		!(arrive > 0.0f && goalDistance < arrive);
	bool backTurnStalled = false;
	if (mayBack || v.cvBackUntil > 0.0f)
	{
		// A CV route can start with a detour point behind the vehicle. Swinging
		// forward toward it takes more room than backing up while there is room behind.
		const float backTurnAngle = v.commanderCV && onRoute && target == nullptr && speed < 60.0f ? 90.0f : 110.0f;
		const bool wantBack = std::fabs(err) > backTurnAngle || (!headingSafe && forwardSpeed < 60.0f);
		// Not while still rolling back from the last one: palmbay 20261122-124651 restarted at
		// -273 u/s, reached ~480 backwards and went over the edge behind (123 such starts in old runs).
		if (mayBack && v.cvBackUntil == 0.0f && now >= v.cvBackNext && wantBack && forwardSpeed < 200.0f &&
			forwardSpeed > -150.0f && rearSafe && found)
		{
			v.cvBackUntil = now + 3.0f;
			v.cvBackStart = origin;
			RCBOT_DIAG("[RCBOT2] vehicle back-turn start userid=%d err=%.0f speed=%.0f ahead_safe=%d\n", userId, err,
				forwardSpeed, headingSafe ? 1 : 0);
		}
		if (v.cvBackUntil > 0.0f)
		{
			const bool aligned = std::fabs(err) < 60.0f && headingSafe;
			if (aligned || now > v.cvBackUntil || !rearSafe)
			{
				// A blocked reverse can expire without ever reaching the forward stuck check.
				backTurnStalled = mayBack && onRoute && target == nullptr && !aligned && rearSafe &&
					Dist2D(origin, v.cvBackStart) < 40.0f;
				RCBOT_DIAG("[RCBOT2] vehicle back-turn end userid=%d err=%.0f reason=%s\n", userId, err,
					aligned ? "aligned" : !rearSafe ? "rear" : backTurnStalled ? "stalled" : "time");
				v.cvBackUntil = 0.0f;
				v.cvBackNext = now + 2.0f;
			}
			else
			{
				// Cap the reverse speed: the rear check looks only 220-360 behind the tail.
				forward = forwardSpeed < -250.0f ? 0.0f : -kMaxMove;
				side = err > 0.0f ? kMaxMove : -kMaxMove;
			}
		}
	}
	if (arrive > 0.0f && goalDistance < arrive)
	{
		forward = 0.0f;
		// Stopping to shell a stationary building: brake cleanly instead of coasting past.
		if (armed && target != nullptr && !v.retreating && !v.commanderCV)
		{
			side = 0.0f;
			if (forwardSpeed > 60.0f)
				forward = -kMaxMove * 0.6f;
			else if (forwardSpeed < -60.0f)
				forward = kMaxMove * 0.6f;
		}
	}
	// Parking for repairs: coasting in at 300-500 u/s with full steering
	// circled the park spot for minutes (run 134810, dist 220-400, err ~100).
	// Slow down on the way in, then stop dead without steering.
	if (v.retreating && !v.padInside && arrive > 0.0f)
	{
		if (goalDistance < arrive + 400.0f && forwardSpeed > 250.0f)
			forward = -kMaxMove * 0.6f;
		if (goalDistance < arrive)
		{
			side = 0.0f;
			forward = forwardSpeed > 60.0f ? -kMaxMove * 0.6f : forwardSpeed < -60.0f ? kMaxMove * 0.6f : 0.0f;
		}
	}
	// Commander CV at its goal: slow down on the way in, stop dead, then handbrake.
	// The slow-down measures to cvGoal itself: arrive stays 0 while a route is followed (route
	// dropped only inside 300), so it started at 300 and the CV reached its Palmbay site goal at
	// 520-570 u/s, overshot 250-340 onto the slope behind it and slid to the low ground for good
	// (runs 20261120-182055, -205131; BE stall 20261121-161716 at 443 u/s).
	if (v.commanderCV && target == nullptr)
	{
		if (Dist2D(origin, v.cvGoal) < v.cvArrive + 500.0f && forwardSpeed > 250.0f)
			forward = -kMaxMove * 0.6f;
	}
	if (v.commanderCV && target == nullptr && arrive > 0.0f)
	{
		if (goalDistance < arrive)
		{
			side = 0.0f;
			forward = forwardSpeed > 60.0f ? -kMaxMove * 0.6f : forwardSpeed < -60.0f ? kMaxMove * 0.6f : 0.0f;
			if (std::fabs(forwardSpeed) < 60.0f && !v.cvArrived &&
				(!v.cvHunt || (v.cvNoPreySince > 0.0f && now - v.cvNoPreySince > 1.5f)))
			{
				v.cvArrived = true;
				v.cvEnd = "arrived";
				RCBOT_DIAG("[RCBOT2] cv-drive arrived userid=%d pos=(%.0f %.0f %.0f) dist=%.0f tick=%d\n", userId, origin.x,
					origin.y, origin.z, goalDistance, gpGlobals->tickcount);
			}
		}
	}
	if (lawlietHold)
	{
		side = 0.0f;
		forward = forwardSpeed > 60.0f ? -kMaxMove * 0.6f : forwardSpeed < -60.0f ? kMaxMove * 0.6f : 0.0f;
	}
	// On the repair pad: creep and stop at the centre (run 20261112-121820
	// crossed it at 350 u/s and swung into the station walls).
	if (v.retreating && v.padInside)
	{
		forward = (std::min)(forward, kMaxMove * (goalDistance < 500.0f ? 0.3f : 0.5f));
		if (goalDistance < 350.0f && forwardSpeed > 120.0f)
			forward = -kMaxMove * 0.6f;
	}
	// Bill Nye lining up behind a vehicle: slow down so the turn-around does
	// not carry the jeep off the end of the road.
	if (v.billNye && targetIsVehicle && !v.ramming && goalDistance < 1500.0f && forwardSpeed > 550.0f)
		forward = -kMaxMove;
	// Heading toward an edge: no creeping on (brake from 60 u/s, run 170711), unless backing up.
	if ((!found || (!headingSafe && forwardSpeed > 60.0f)) && v.cvBackUntil == 0.0f)
	{
		forward = -kMaxMove; // brake hard (reverse throttle brakes first)
		if (forwardSpeed < -80.0f)
			forward = kMaxMove; // rolling backwards: brake that instead
		else if (forwardSpeed < 80.0f && !rearSafe)
			forward = 0.0f; // stopped: reversing would go over the rear edge
		if (!found)
		{
			const float leftDrop = DropDistance(origin, heading + 90.0f, 900.0f);
			const float rightDrop = DropDistance(origin, heading - 90.0f, 900.0f);
			side = leftDrop > rightDrop ? -kMaxMove : kMaxMove;
		}
	}

	const bool haltedByEdge = !headingSafe && !v.billNye && !pivots && forward > 0.0f && v.cvBackUntil == 0.0f;
	if (haltedByEdge)
		forward = 0.0f; // never creep on toward an edge; the back-turn follows
	// Tactical combat movement: keep speed moderate (~260 u/s) so cannon aim stays steady
	// and vehicles do not fly over edges or behind cover while fighting.
	if (v.fightMove && forward > 0.0f)
	{
		if (forwardSpeed > 380.0f)
			forward = -kMaxMove * 0.5f;
		else if (forwardSpeed > 260.0f)
			forward = 0.0f;
	}

	if (facing)
	{
		// Turn with a little throttle (wheels need it), then hold still.
		forward = std::fabs(err) > 15.0f && !pivots ? kMaxMove * 0.35f : 0.0f;
		if (std::fabs(forwardSpeed) > 250.0f)
			forward = -kMaxMove * 0.5f;
		if (now >= v.nextFaceLog)
		{
			v.nextFaceLog = now + 3.0f;
			RCBOT_DIAG("[RCBOT2] vehicle face-side userid=%d veh=%d side=%d weak=%d armor=%.2f/%.2f err=%.0f\n",
				userId, v.vehicleIndex, bestSide, worstSide, bestArmor, worstArmor, err);
		}
	}

	if (applyManeuver)
	{
		v.cvBackUntil = 0.0f;
		const float requested = v.maneuverSpeed > 0.0f && curveSpeed > 0.0f ?
			(std::min)(v.maneuverSpeed, curveSpeed) : v.maneuverSpeed;
		side = kMaxMove * v.maneuverSteer;
		const float error = requested - forwardSpeed;
		forward = kMaxMove * (std::max)(-1.0f, (std::min)(1.0f, error / 100.0f));
		if (requested == 0.0f && speed < 30.0f)
			forward = 0.0f;
	}

	// A rocking tank can repeatedly cross the speed threshold without moving a hull length.
	bool padStalled = false;
	if (!drivingToPad || lawlietHold)
		v.padProgressSince = -1.0f;
	else if (v.padProgressSince < 0.0f || Dist2D(origin, v.padProgressOrigin) >= 96.0f)
	{
		v.padProgressSince = now;
		v.padProgressOrigin = origin;
	}
	else if (now - v.padProgressSince >= 8.0f && now >= v.reverseUntil)
	{
		padStalled = true;
		v.padProgressSince = now;
		RCBOT_DIAG("[RCBOT2] stall-escalate who=vehicle stage=1 reason=pad-no-progress userid=%d veh=%d pos=(%.0f %.0f) moved=%.0f\n",
			userId, v.vehicleIndex, origin.x, origin.y, Dist2D(origin, v.padProgressOrigin));
		// TODO K6 (human test 26.11., crossroads 20261126-203307: NF heavy ~2 min at its own gate pillar on
		// the way to the pad, only stage 1 five times): next entrance after 2 stalls, give up after 4.
		if (rcbot_empires_pad_stall_escalate.GetBool())
		{
			++v.padStalls;
			if (v.padStalls >= 4)
			{
				v.padGiveUp = true;
				RCBOT_DIAG("[RCBOT2] stall-escalate who=vehicle stage=3 reason=pad-no-progress-give-up userid=%d veh=%d stalls=%d\n",
					userId, v.vehicleIndex, v.padStalls);
			}
			else if (v.padStalls == 2)
			{
				v.padStation = -1;
				++v.padEntryAttempt;
				v.nextReplan = 0.0f;
				RCBOT_DIAG("[RCBOT2] stall-escalate who=vehicle stage=2 reason=pad-no-progress-next-entry userid=%d veh=%d try=%d\n",
					userId, v.vehicleIndex, v.padEntryAttempt);
			}
		}
	}

	// Stuck while throttling: back off with the wheels turned the other way.
	if (!applyManeuver && now < v.reverseUntil)
	{
		if (rearSafe)
		{
			forward = -kMaxMove;
			side = v.reverseSide;
		}
		else
		{
			v.reverseUntil = 0.0f; // no ground behind: stop instead
			forward = 0.0f;
		}
	}
	else if (!lawlietHold && (padStalled || backTurnStalled || (speed < 40.0f &&
		(applyManeuver || ((forward > 0.0f || haltedByEdge) && !facing && !(v.billNye && v.ramming))))))
	{
		if (!padStalled && !backTurnStalled && v.slowSince == 0.0f)
			v.slowSince = now;
		else if (padStalled || backTurnStalled || now - v.slowSince > 2.5f)
		{
			v.slowSince = 0.0f;
			++v.stuckReverses;
			if (v.retreating && !v.parkValid && v.stuckReverses >= 4)
			{
				v.parkValid = true;
				v.park = origin;
				v.parkSince = now;
				++v.parkCount;
				v.stuckReverses = 0;
				RCBOT_DIAG("[RCBOT2] vehicle retreat-park userid=%d veh=%d pos=(%.0f %.0f %.0f)\n", userId,
					v.vehicleIndex, origin.x, origin.y, origin.z);
			}
			bool clearedForExit = false;
			if (v.stuckReverses > 4 && v.commanderCV)
			{
				// The commander keeps his CV: stop here, the economy decides (walk or give up).
				v.cvArrived = true;
				v.cvEnd = "stuck";
				RCBOT_DIAG("[RCBOT2] cv-drive stuck userid=%d pos=(%.0f %.0f %.0f)\n", userId, origin.x, origin.y, origin.z);
			}
			else if (v.stuckReverses > 4 && !(v.billNye && v.ramming))
			{
				edict_t* ownVf = nullptr;
				Vector ownVfOrigin;
				int ownVfTeam = -1;
				const bool inBay = v.exitUntil > 0.0f ||
					(findEmpiresVehicleFactory(team, ownVf, ownVfOrigin, ownVfTeam) &&
					 IsNearFactoryBay(ownVf, origin, 400.0f));
				// 3.26 (3b): before abandoning a tank in a narrow gap or factory bay, pull back
				// longer and replan fresh so it either frees itself or clears the passage.
				if (!v.clearBeforeExit && (inBay || rearSafe))
				{
					clearedForExit = true;
					v.clearBeforeExit = true;
					v.route.clear();
					v.routeAreas.clear();
					v.nextReplan = now + 3.0f;
					if (rearSafe)
					{
						v.reverseUntil = now + 3.0f;
						v.reverseSide = side >= 0.0f ? -kMaxMove : kMaxMove;
						forward = -kMaxMove;
						side = v.reverseSide;
					}
					RCBOT_DIAG("[RCBOT2] stall-escalate who=vehicle userid=%d stage=1 reason=clear-before-exit veh=%d pos=(%.0f %.0f %.0f) in_bay=%d rear_safe=%d\n",
						userId, v.vehicleIndex, origin.x, origin.y, origin.z, inBay ? 1 : 0, rearSafe ? 1 : 0);
				}
				else if (!inBay || v.stuckReverses > 8)
				{
					RCBOT_DIAG("[RCBOT2] stall-escalate who=vehicle userid=%d stage=2 reason=give-up-stuck veh=%d pos=(%.0f %.0f %.0f) reverses=%d in_bay=%d\n",
						userId, v.vehicleIndex, origin.x, origin.y, origin.z, v.stuckReverses - 1, inBay ? 1 : 0);
					RCBOT_DIAG("[RCBOT2] vehicle give-up userid=%d veh=%d pos=(%.0f %.0f %.0f) reverses=%d\n", userId,
						v.vehicleIndex, origin.x, origin.y, origin.z, v.stuckReverses - 1);
					RecordBotVehicleAbandon(vehicle, v.vehicleIndex, userId, origin, now);
					v.abandonedVehicle = v.vehicleIndex;
					v.abandonedUntil = now + 120.0f;
					v.nextAction = now;
					setStage(EmpiresVehicleStage::GIVE_UP_EXIT, 10.0f);
					return;
				}
			}
			if (!clearedForExit && applyManeuver)
			{
				// A failed approach says nothing about the next navmesh portal.
				v.maneuverRecoverUntil = now + 1.5f;
				v.nextManeuver = 0.0f;
				v.nextReplan = now;
				const edict_t* blocker = v.maneuverBlocker > 0 ? engine->PEntityOfEntIndex(v.maneuverBlocker) : nullptr;
				const char* blockerClass = blocker != nullptr && !blocker->IsFree() && blocker->GetClassName() != nullptr ?
					blocker->GetClassName() : (v.maneuverBlocker == 0 ? "world" : "unknown");
				RCBOT_DIAG("[RCBOT2] vehicle maneuver-retry userid=%d pos=(%.0f %.0f %.0f) blocker=%d class=%s n=%d\n",
					userId, origin.x, origin.y, origin.z, v.maneuverBlocker, blockerClass, v.stuckReverses);
				if (v.detourActive)
				{
					RCBOT_DIAG("[RCBOT2] vehicle detour end userid=%d entity=%d reason=retry pos=(%.0f %.0f %.0f) speed=%.0f\n",
						userId, v.detourEntity, origin.x, origin.y, origin.z, speed);
					v.detourActive = false;
					v.maneuverValid = false;
				}

				// The navmesh does not know entities. A built engineer barricade
				// (BE VF run 20261119-141352: three retries at emp_eng_map_brush, the
				// replanned route went through it again), or a second stall, closes
				// the edge like the regular stuck path does.
				const bool barricade = blocker != nullptr && blocker->GetClassName() != nullptr &&
					std::strncmp(blocker->GetClassName(), "emp_eng_map_", 12) == 0;
				edict_t* barricadeEdict = barricade ? engine->PEntityOfEntIndex(v.maneuverBlocker) : nullptr;
				if (barricadeEdict != nullptr && barricadeEdict->GetCollideable() != nullptr)
				{
					// It spans the road across several areas: one closed link
					// only moved the route to the next link through it (BE VF run
					// 20261119-142253). Close every link through its box for 60 s;
					// it can be destroyed and rebuilt.
					Vector boxMins, boxMaxs;
					barricadeEdict->GetCollideable()->WorldSpaceSurroundingBounds(&boxMins, &boxMaxs);
					const Vector pad(v.width * 0.5f, v.width * 0.5f, 0.0f);
					const int links = RCBotNavMesh::BlockEdgesThroughBox(boxMins - pad, boxMaxs + pad, 60.0f);
					RCBOT_DIAG("[RCBOT2] vehicle route-block userid=%d pos=(%.0f %.0f %.0f) links=%d box=(%.0f %.0f)-(%.0f %.0f) reason=maneuver-barricade\n",
						userId, origin.x, origin.y, origin.z, links, boxMins.x, boxMins.y, boxMaxs.x, boxMaxs.y);
				}
				else if (onRoute && v.stuckReverses >= 2)
				{
					const int edge = RCBotNavMesh::BlockVehicleEdgeAt(origin, v.routeAreas, 90.0f);
					RCBOT_DIAG("[RCBOT2] vehicle route-block userid=%d pos=(%.0f %.0f %.0f) edge=%d areas=%zu reason=maneuver-repeat\n",
						userId, origin.x, origin.y, origin.z, edge, v.routeAreas.size());
				}
			}
			else if (!clearedForExit)
			{
				if (onRoute)
				{
					const int edge = RCBotNavMesh::BlockVehicleEdgeAt(origin, v.routeAreas, 90.0f);
					RCBOT_DIAG("[RCBOT2] vehicle route-block userid=%d pos=(%.0f %.0f %.0f) edge=%d areas=%zu\n", userId,
						origin.x, origin.y, origin.z, edge, v.routeAreas.size());
					v.nextReplan = backTurnStalled ? now : now + 1.5f;
				}
				if (backTurnStalled)
					RCBOT_DIAG("[RCBOT2] vehicle stuck-back-turn userid=%d pos=(%.0f %.0f %.0f) n=%d\n", userId,
						origin.x, origin.y, origin.z, v.stuckReverses);
				else if (rearSafe)
				{
					v.reverseUntil = now + 1.5f;
					v.reverseSide = side >= 0.0f ? -kMaxMove : kMaxMove;
					if (v.fightMove)
						v.fightSwitch = now; // blocked while circling: other way round
					RCBOT_DIAG("[RCBOT2] vehicle stuck-reverse userid=%d pos=(%.0f %.0f %.0f) n=%d tick=%d\n", userId,
						origin.x, origin.y, origin.z, v.stuckReverses, gpGlobals->tickcount);
					// What holds the vehicle (TODO 3.26: Palmbay tanks gave up on open mesh between palms).
					LogFactoryExitHull(userId, m_pEdict, vehicle, origin, heading, 80.0f, "stuck-front");
				}
				else
					RCBOT_DIAG("[RCBOT2] vehicle stuck-no-rear userid=%d pos=(%.0f %.0f %.0f) n=%d\n", userId,
						origin.x, origin.y, origin.z, v.stuckReverses);
			}
		}
	}
	else
		v.slowSince = 0.0f;
	// Free driving for 5 s clears the stuck count.
	if (speed > 200.0f)
	{
		if (v.movingSince == 0.0f)
			v.movingSince = now;
		else if (now - v.movingSince > 5.0f)
		{
			v.stuckReverses = 0;
			v.clearBeforeExit = false;
			ClearBotVehicleAbandonIfMoved(vehicle, v.vehicleIndex, origin);
		}
	}
	else
		v.movingSince = 0.0f;

	int buttons = 0;
	if (lawlietHold)
	{
		forward = forwardSpeed > 60.0f ? -kMaxMove * 0.6f : forwardSpeed < -60.0f ? kMaxMove * 0.6f : 0.0f;
		side = 0.0f;
		if (std::fabs(forwardSpeed) < 120.0f)
			buttons |= 0x08000000;
	}
	if (applyManeuver && v.maneuverSpeed == 0.0f && speed < 30.0f)
		buttons |= 0x08000000;
	if (forward == 0.0f && std::fabs(forwardSpeed) < 80.0f && !rearSafe && (!found || !headingSafe))
		buttons |= 0x08000000; // handbrake: hold still at the edge
	if (v.retreating && forward == 0.0f && std::fabs(forwardSpeed) < 120.0f)
		buttons |= 0x08000000; // parked for repairs
	if (v.cvArrived)
	{
		forward = 0.0f;
		side = 0.0f;
		buttons |= 0x08000000; // CV holds still; exit needs a slow vehicle
	}
	if (ram && std::fabs(err) < 15.0f && forward > 0.0f)
		buttons |= IN_SPEED; // boost into the target
	// Boomtanking: a sharp turn at speed swings the tail round with the handbrake
	// instead of a wide arc past the target.
	if (v.commanderCV && ram && found && std::fabs(err) > 55.0f && forwardSpeed > 300.0f)
	{
		buttons |= 0x08000000;
		forward = kMaxMove;
	}

	// -------- aim and fire
	// The tank turret reads the command's view angles relative to the hull, not as world angles
	// (run 130949: aim 199.5 world, hull model yaw 170.6, shells flew 10 = 170.6 + 199.5; every
	// cannon and MG shot missed). Turn a world direction into the hull frame before sending it.
	const QAngle hullAngles = vehicle->GetCollideable() != nullptr ? vehicle->GetCollideable()->GetCollisionAngles() :
		QAngle(0, 0, 0);
	auto hullView = [&](const Vector& worldDir) {
		matrix3x4_t hull;
		AngleMatrix(hullAngles, hull);
		Vector local;
		VectorIRotate(worldDir, hull, local);
		QAngle out;
		VectorAngles(local, out);
		out.z = 0.0f;
		return out;
	};
	QAngle view = hullView(YawDir(chosen)); // no target: turret along the driving direction
	if (rcbot_empires_vehicle_shell_trace.GetBool())
		TraceShells(userId, target);
	if (target != nullptr)
	{
		// 3.21a: every turret weapon converges on the point the cannon axis hits
		// (CEmpTank::GetWeaponVectors 0x69add0), and the axis runs through the cannon pivot
		// parallel to the view direction. Aim from the pivot, not from hull origin + 90: the NF
		// heavy/medium/APC pivots sit 130-151 above the hull origin, so the MG went over every
		// head (NF heavy 1.6 HP/s vs BE heavy 28.3, RE vehicle-driving.md).
		// 3.21a: the pivot resolved once above (cannonEye/aimCannon), same for sight, movement and fire.
		const Vector eye = cannonEye;
		edict_t* const aimCannon = cannonEdict;
		const int targetIndex = engine->IndexOfEdict(target);
		const bool infantry = targetIndex >= 1 && targetIndex <= gpGlobals->maxClients;
		// Lead: target velocity from its last positions (smoothed).
		Vector centre = EntityCenter(target);
		const Vector normalCentre = centre;
		bool mgLowAimActive = false;
		// Human test 26.11.: a soldier revived lying keeps a standing-height box, the MG aimed at its
		// centre and fired over him for seconds. Like a gunner who sees no hits: after 1.2 s of MG fire
		// without damage aim at the body on the ground, after 3 s take another target for 10 s.
		// Never lower aim while pitch-limited (already at max depression -> lowering only deadlocks).
		if (infantry && rcbot_empires_vehicle_mg_no_damage.GetBool() && v.mgEngTarget == targetIndex &&
			v.mgEngHp0 > 0 && v.mgEngHpLast >= v.mgEngHp0)
		{
			if (v.mgEngFired >= 3.0f && v.runOverTarget != targetIndex)
			{
				RCBOT_DIAG("[RCBOT2] stall-escalate who=vehicle-mg userid=%d stage=2 reason=no-damage-skip target=%d "
					"fired=%.1f hp=%d\n", userId, targetIndex, v.mgEngFired, v.mgEngHpLast);
				v.fireBlockedSkipTarget = targetIndex;
				v.fireBlockedSkipUntil = now + 10.0f;
				v.target = nullptr;           // this frame still finishes with the old target
				v.nextTargetPick = now;       // pick again next frame; inSight() skips this one
				v.mgEngTarget = -1;
			}
			else if (v.mgEngFired >= 1.2f && now >= v.pitchLimitUntil)
			{
				centre.z = CBotGlobals::entityOrigin(target).z + 12.0f;
				mgLowAimActive = true;
				if (v.mgLowAimLogged != targetIndex)
				{
					v.mgLowAimLogged = targetIndex;
					RCBOT_DIAG("[RCBOT2] stall-escalate who=vehicle-mg userid=%d stage=1 reason=no-damage-aim-low target=%d "
						"fired=%.1f hp=%d\n", userId, targetIndex, v.mgEngFired, v.mgEngHpLast);
				}
			}
		}
		if (targetIndex != v.aimTarget)
		{
			v.aimTarget = targetIndex;
			v.aimVel = Vector(0, 0, 0);
			v.aimLastPos = normalCentre;
			v.aimLastTime = now;
		}
		else if (now - v.aimLastTime >= 0.2f)
		{
			const Vector measured = (normalCentre - v.aimLastPos) / (now - v.aimLastTime);
			v.aimVel = measured.Length() < 2500.0f ? v.aimVel * 0.5f + measured * 0.5f : Vector(0, 0, 0);
			v.aimLastPos = normalCentre;
			v.aimLastTime = now;
		}
		// vehicle_weapons.txt: "Speed"/"Gravity" apply only to projectile types 1-4; machine guns
		// (type 0) fire hitscan bullets with a tracer, so no lead on infantry (3.21: with the old
		// range/1000 lead, moving infantry at 500-900 took 1.5 HP/s vs 18.9 standing, run 211230).
		// Standard Cannon 3300 u/s, gravity 0.5, splash 250.
		// 3.10f: with only missiles loaded in the active group (no cannon), lead at 2300 u/s without drop.
		const float range = (centre - eye).Length();
		const EmpiresVehicleAim::BallisticsResult ballistics = EmpiresVehicleAim::ChooseBallistics(range, infantry,
			!v.weaponsKnown || v.weaponActiveCannonLoaded, v.weaponsKnown && v.weaponActiveMissileLoaded);
		const float flight = ballistics.flightTime;
		Vector aim = centre + v.aimVel * flight;
		aim.z += ballistics.gravityDropZ; // drop at sv_gravity 600 (assumed), 0 for MG and missiles
		QAngle worldView;
		VectorAngles(aim - eye, worldView);
		// Offset-MG parallax compensation: CEmpTank::GetWeaponVectors (0x69af23) ignores players
		// (CEmpGameRules::ShouldCollide(8, 14) == 0), so tanks with offset MGs (NF Heavy -31.2 HU
		// left, BE Med +30 HU right, NF Med +20 HU up) converge on the ground behind a soldier and
		// miss him laterally/vertically. Trace past the soldier to find that convergence point and
		// shift the cannon aim so the MG ray passes through his chest.
		if (infantry && !mgLowAimActive && range > 1.0f && enginetrace != nullptr &&
			EmpiresVehicleAim::HasMgParallaxOffset(chassis, false))
		{
			const Vector rayDir = (aim - eye) / range;
			Ray_t behindRay;
			behindRay.Init(aim, aim + rayDir * 4000.0f);
			CTraceFilterNoPlayers behindFilter;
			trace_t behindHit;
			enginetrace->TraceRay(behindRay, MASK_SOLID, &behindFilter, &behindHit);
			const float behindDist = (!behindHit.startsolid && behindHit.fraction < 1.0f)
				? (behindHit.fraction * 4000.0f) : -1.0f;
			float rightShift = 0.0f, upShift = 0.0f;
			EmpiresVehicleAim::MgParallaxAimShift(chassis, false, range, behindDist, rightShift, upShift);
			if (rightShift != 0.0f || upShift != 0.0f)
			{
				Vector fwdVec, rightVec, upVec;
				AngleVectors(worldView, &fwdVec, &rightVec, &upVec);
				aim += rightVec * rightShift + upVec * upShift;
				const float minAimZ = CBotGlobals::entityOrigin(target).z + 18.0f;
				if (aim.z < minAimZ)
					aim.z = minAimZ;
				VectorAngles(aim - eye, worldView);
			}
		}
		view = hullView(aim - eye);
		// Minimum range (user 25.11.: every vehicle has its own distance below which it cannot aim
		// at the ground or at players): the game clamps the requested cannon pitch to the
		// chassis limits (CEmpTankCannon::SetTargetRotation 0x3e6d50, front/rear blended), and
		// every turret weapon follows the cannon axis. A clamped pitch means the target is out of
		// reach: hold fire and open the range instead of wasting rounds into the ground.
		// Check pitch limits against normalCentre so no-damage-aim-low cannot self-lock into pitch-limit.
		if (aimCannon != nullptr && rcbot_empires_vehicle_pitch_limit.GetBool() && now - v.aimSince > 0.8f)
		{
			// m_fDesiredLocalPitch reads 0 on the server (runs 130931/130951); the cannon's limits are
			// readable, so clamp the request ourselves (Source pitch: positive = down).
			float minFront = 0.0f, maxFront = 0.0f;
			if (ReadEntityFloat(aimCannon, "m_fMinPitch", minFront) && ReadEntityFloat(aimCannon, "m_fMaxPitch", maxFront) &&
				maxFront > minFront)
			{
				float minRear = minFront, maxRear = maxFront;
				ReadEntityFloat(aimCannon, "m_fMinPitchRear", minRear);
				ReadEntityFloat(aimCannon, "m_fMaxPitchRear", maxRear);
				const QAngle pitchCheckView = mgLowAimActive ? hullView(normalCentre - eye) : view;
				float minPitch = minFront, maxPitch = maxFront;
				EmpiresVehicleAim::BlendedPitchLimits(pitchCheckView.y, minFront, maxFront, minRear, maxRear, minPitch, maxPitch);
				if (v.pitchRangeLogged != v.vehicleIndex)
				{
					v.pitchRangeLogged = v.vehicleIndex;
					RCBOT_DIAG("[RCBOT2] vehicle pitch-range userid=%d veh=%d chassis=%d min=%.1f max=%.1f rear=(%.1f %.1f) pivot_dz=%.0f\n",
						userId, v.vehicleIndex, chassis, minFront, maxFront, minRear, maxRear, eye.z - origin.z);
				}
				const float wantPitch = NormalizeYaw(pitchCheckView.x);
				const float gotPitch = (std::min)((std::max)(wantPitch, minPitch), maxPitch);
				const float miss = wantPitch - gotPitch; // > 0: wanted further down
				const float pitchThreshold = infantry ? 2.0f : 3.0f;
				if (std::fabs(miss) > pitchThreshold)
				{
					v.pitchLimitUntil = now + 1.5f;
					v.pitchLimitDown = miss > 0.0f;
					if (now >= v.pitchLimitLogAt)
					{
						v.pitchLimitLogAt = now + 5.0f;
						RCBOT_DIAG("[RCBOT2] vehicle pitch-limit userid=%d chassis=%d target=%d dist=%.0f dz=%.0f want=%.1f got=%.1f %s\n",
							userId, chassis, targetIndex, Dist2D(origin, normalCentre), eye.z - normalCentre.z, wantPitch,
							gotPitch, v.pitchLimitDown ? "too-close" : "too-high");
					}
				}
			}
		}
		if (armed)
		{
			float heat = 0.0f;
			ReadEntityFloat(vehicle, "m_flVehicleHeat", heat);
			const float distance = Dist2D(origin, targetAt);
			const char* blockWhy = "";
			float blockFrac = 1.0f;
			int blockIndex = -1;
			Vector blockEnd(0, 0, 0);
			const bool lineClear = HasClearFireLine(m_pEdict, vehicle, eye, target, &blockWhy, &blockFrac,
				&blockIndex, &blockEnd);
			if (lineClear)
			{
				v.fightSight = now;
				v.fireBlockedSince = 0.0f;
				v.fireBlockedStage = 0;
				v.fireBlockedFlankUntil = 0.0f;
			}
			else if (!infantry && distance < 1500.0f)
			{
				if (v.fireBlockedTarget != targetIndex || v.fireBlockedSince <= 0.0f)
				{
					v.fireBlockedTarget = targetIndex;
					v.fireBlockedSince = now;
					v.fireBlockedStage = 0;
				}
				const float blockedFor = now - v.fireBlockedSince;
				edict_t* blockEdict = blockIndex > 0 ? engine->PEntityOfEntIndex(blockIndex) : nullptr;
				const char* blockCls = blockEdict != nullptr && blockEdict->GetClassName() != nullptr ?
					blockEdict->GetClassName() : "-";
				if (v.fireBlockedStage == 0 && blockedFor >= 6.0f)
				{
					v.fireBlockedStage = 1;
					v.fightDir = v.fightDir == 0.0f ? (randomFloat(0.0f, 1.0f) < 0.5f ? 1.0f : -1.0f) : -v.fightDir;
					v.fireBlockedFlankUntil = now + 6.0f;
					RCBOT_DIAG("[RCBOT2] stall-escalate who=vehicle userid=%d stage=1 reason=fire-blocked target=%d dist=%.0f "
						"by=%s blocker=%d(%s)\n", userId, targetIndex, distance, blockWhy, blockIndex, blockCls);
				}
				else if (v.fireBlockedStage == 1 && blockedFor >= 14.0f)
				{
					v.fireBlockedStage = 2;
					v.fireBlockedSkipTarget = targetIndex;
					v.fireBlockedSkipUntil = now + 15.0f;
					v.fireBlockedSince = 0.0f;
					v.fireBlockedFlankUntil = 0.0f;
					v.target = nullptr;
					RCBOT_DIAG("[RCBOT2] stall-escalate who=vehicle userid=%d stage=2 reason=fire-blocked-skip target=%d dist=%.0f "
						"by=%s blocker=%d(%s)\n", userId, targetIndex, distance, blockWhy, blockIndex, blockCls);
				}
				if (now >= v.nextAimLog)
				{
					v.nextAimLog = now + 5.0f;
					RCBOT_DIAG("[RCBOT2] vehicle fire-blocked userid=%d target=%d dist=%.0f by=%s frac=%.2f blocker=%d(%s) "
						"eye=(%.0f %.0f %.0f) end=(%.0f %.0f %.0f)\n", userId, targetIndex, distance, blockWhy, blockFrac,
						blockIndex, blockCls, eye.x, eye.y, eye.z, blockEnd.x, blockEnd.y, blockEnd.z);
				}
			}
			// 3.21a: fire once the turret really points at the target instead of a blind 1.2 s.
			// Chassis without a rotatable turret (no turret, or m_fMinYaw == m_fMaxYaw) keep the delay.
			bool aimReady = false;
			{
				int alignTurretIndex = -1;
				edict_t* alignTurret = readEmpiresEntityHandleProperty(vehicle, "m_hTurret", 0, alignTurretIndex) &&
					alignTurretIndex > 0 && alignTurretIndex < gpGlobals->maxEntities ?
					engine->PEntityOfEntIndex(alignTurretIndex) : nullptr;
				float minYaw = 0.0f, maxYaw = 0.0f;
				const bool yawRangeKnown = alignTurret != nullptr && !alignTurret->IsFree() &&
					ReadEntityFloat(alignTurret, "m_fMinYaw", minYaw) && ReadEntityFloat(alignTurret, "m_fMaxYaw", maxYaw);
				EmpiresVehicleAim::TurretAlignInput alignIn;
				alignIn.alignCheckEnabled = rcbot_empires_vehicle_turret_align.GetBool();
				alignIn.hasRotatableTurret = EmpiresVehicleAim::HasRotatableTurret(yawRangeKnown, minYaw, maxYaw);
				// turret models face local +X: collision yaw = barrel world yaw (runs 121856/121916)
				alignIn.turretYawDeg = alignIn.hasRotatableTurret && alignTurret->GetCollideable() != nullptr ?
					alignTurret->GetCollideable()->GetCollisionAngles().y : 0.0f;
				alignIn.wantYawDeg = worldView.y;
				alignIn.aimedForSeconds = now - v.aimSince;
				alignIn.infantryTarget = infantry;
				alignIn.targetRange = range;
				const EmpiresVehicleAim::TurretAlignResult align = EmpiresVehicleAim::TurretAlignedToFire(alignIn);
				aimReady = align.canFire;
				if (lineClear && align.fallbackEscalated &&
					(v.turretAlignTarget != targetIndex || now >= v.turretAlignLogAt))
				{
					v.turretAlignTarget = targetIndex;
					v.turretAlignLogAt = now + 10.0f;
					RCBOT_DIAG("[RCBOT2] stall-escalate who=vehicle-aim userid=%d stage=1 reason=turret-not-aligned "
						"target=%d chassis=%d yaw_err=%.0f aimed_for=%.1f yaw_range=(%.0f %.0f)\n", userId, targetIndex,
						chassis, align.yawErrorDeg, alignIn.aimedForSeconds, minYaw, maxYaw);
				}
			}
			if (lineClear && aimReady && heat < 80.0f && now >= v.pitchLimitUntil)
			{
				// Fallback when the turret arrays cannot be read: the bot's own loadout has the MG on
				// IN_ATTACK and the cannon on IN_ATTACK2 (run 150322: MG ammo drops under IN_ATTACK,
				// cannon 40 -> 36 under IN_ATTACK2).
				const int kMachineGun = IN_ATTACK;
				const int kCannon = IN_ATTACK2;
				if (now - v.weaponReadAt >= 0.25f || v.weaponReadAt > now)
				{
					v.weaponReadAt = now;
					int turretIndex = -1;
					edict_t* turret = readEmpiresEntityHandleProperty(vehicle, "m_hTurret", 0, turretIndex) &&
						turretIndex > 0 && turretIndex < gpGlobals->maxEntities ? engine->PEntityOfEntIndex(turretIndex) : nullptr;
					ReadVehicleWeapons(this, turret, v);
				}
				// 3.10f: fire group 1 (index 0: cannon + missile + MG) against vehicles and
				// buildings, group 2 (index 1: cannon + MG) against infantry, so the missiles are
				// kept for armour. emp_change_vehicweap <1..6> selects group N-1 for the driver
				// (CEmpPlayer::ChangeVehicleWeapon 0x663160; ignored for an empty group).
				// 3.10f: no shot in the switch tick nor in the 0.35 s until the turret reports the new
				// group (it would fire the old group: missiles at infantry, or no missile at armour).
				bool fireGroupHold = false;
				if (helpers != nullptr)
				{
					const EmpiresVehicleAim::FireGroupDecision groupDecision = EmpiresVehicleAim::DecideFireGroupSwitch(
						v.weaponsKnown, v.weaponGroup, v.weaponGroupCount, rcbot_empires_vehicle_missiles.GetBool(),
						infantry, now, v.fireGroupSentAt);
					fireGroupHold = groupDecision.suppressFireThisTick;
					if (fireGroupHold)
						++v.fireGroupHeld;
					const int wantGroup = groupDecision.wantGroup;
					if (groupDecision.sendSwitch)
					{
						v.fireGroupSentAt = now;
						char cmd[48];
						snprintf(cmd, sizeof(cmd), "emp_change_vehicweap %d\n", wantGroup + 1);
						helpers->ClientCommand(m_pEdict, cmd);
						RCBOT_DIAG("[RCBOT2] vehicle fire-group userid=%d from=%d to=%d target=%d infantry=%d dist=%.0f "
							"held_before=%d\n", userId, v.weaponGroup + 1, wantGroup + 1, targetIndex, infantry ? 1 : 0, distance,
							v.fireGroupHeld);
						v.fireGroupHeld = 1; // this tick
						v.weaponReadAt = -1.0f; // read the new group on the next tick
					}
				}
				const char* why = "fallback";
				if (v.weaponsKnown)
				{
					// Infantry only with the MG, never the cannon (user 15.11., replaces the 14.11.
					// rule "cannon once every MG is empty"); cannon/missiles against vehicles and
					// buildings. Buttons that would also fire a cannon are not used on infantry.
					if (infantry)
					{
						if (distance < kVehicleMgInfantryRange + 200.0f && v.weaponMgPure != 0)
						{
							buttons |= v.weaponMgPure;
							why = "mg";
						}
						else if (v.weaponMgMixed != 0)
							why = "mg-mixed"; // user: never fire a shared MG/cannon button at infantry
						else
							why = v.weaponMgLeft ? "mg-reloading" : "no-weapon";
					}
					else if (distance < 4000.0f && v.weaponArmour != 0)
					{
						buttons |= v.weaponArmour;
						why = "armour";
					}
					else
						why = "no-armour-weapon";
				}
				else if (infantry)
				{
					if (distance < kVehicleMgInfantryRange + 200.0f)
						buttons |= kMachineGun; // MG only on infantry (user 15.11.)
				}
				else if (distance < 4000.0f)
					buttons |= kCannon;
				if (fireGroupHold)
				{
					buttons &= ~(IN_ATTACK | IN_ATTACK2);
					why = "fire-group-switch";
				}
				// 3.21 diagnosis: one line per MG engagement on infantry (fire time, time the target
				// moved > 80 u/s, health at start and end).
				{
					const bool mgFire = infantry && std::strcmp(why, "mg") == 0;
					const bool gap = now - v.mgEngLastTick > 2.0f;
					if (v.mgEngTarget >= 0 && (gap || (mgFire && targetIndex != v.mgEngTarget)))
					{
						RCBOT_DIAG("[RCBOT2] vehicle mg-engage userid=%d target=%d fired=%.1f moving=%.1f dist=%.0f hp=%d->%d "
							"chassis=%d hull_off=%.0f turret_off=%.0f\n",
							userId, v.mgEngTarget, v.mgEngFired, v.mgEngMoving,
							v.mgEngTicks > 0 ? v.mgEngDistSum / v.mgEngTicks : 0.0f, v.mgEngHp0, v.mgEngHpLast, chassis,
							v.mgEngTicks > 0 ? v.mgEngHullOff / v.mgEngTicks : -1.0f,
							v.mgEngTicks > 0 ? v.mgEngTurretOff / v.mgEngTicks : -1.0f);
						v.mgEngTarget = -1;
					}
					if (mgFire)
					{
						IPlayerInfo* tinfo = playerinfomanager != nullptr ? playerinfomanager->GetPlayerInfo(target) : nullptr;
						const int hp = tinfo != nullptr && !tinfo->IsDead() ? tinfo->GetHealth() : 0;
						if (v.mgEngTarget != targetIndex)
						{
							v.mgEngTarget = targetIndex;
							v.mgEngFired = v.mgEngMoving = v.mgEngDistSum = 0.0f;
							v.mgEngTicks = 0;
							v.mgEngHp0 = hp;
							v.mgEngHullOff = v.mgEngTurretOff = 0.0f;
						}
						else
						{
							const float dt = (std::min)(now - v.mgEngLastTick, 0.2f);
							v.mgEngFired += dt;
							if (Vector(v.aimVel.x, v.aimVel.y, 0.0f).Length() > 80.0f)
								v.mgEngMoving += dt;
						}
						v.mgEngHpLast = hp;
						v.mgEngDistSum += distance;
						float signedYawErr = 0.0f;
						int mgClipAmmo = -1;
						{
							const float want = YawTo(origin, centre);
							const QAngle hullAng = vehicle->GetCollideable() != nullptr ?
								vehicle->GetCollideable()->GetCollisionAngles() : QAngle(0, 0, 0);
							v.mgEngHullOff += std::fabs(NormalizeYaw(hullAng.y + 90.0f - want));
							int turretIndex = -1;
							edict_t* turret = readEmpiresEntityHandleProperty(vehicle, "m_hTurret", 0, turretIndex) &&
								turretIndex > 0 && turretIndex < gpGlobals->maxEntities ?
								engine->PEntityOfEntIndex(turretIndex) : nullptr;
							const QAngle turretAng = turret != nullptr && turret->GetCollideable() != nullptr ?
								turret->GetCollideable()->GetCollisionAngles() : QAngle(0, 0, 0);
							// turret models face local +X (measured: +90 read 88-92 while aligned, runs 121856/121916)
							v.mgEngTurretOff += std::fabs(NormalizeYaw(turretAng.y - want));
							signedYawErr = NormalizeYaw(turretAng.y - worldView.y);
							if (rcbot_empires_vehicle_shell_trace.GetBool() && turret != nullptr)
							{
								std::int32_t wIds[8], wAmmo[8];
								if (ReadTurretArray(turret, "m_iWeapon", wIds) && ReadTurretArray(turret, "m_iWeaponAmmo", wAmmo))
								{
									for (int wi = 0; wi < 8; ++wi)
									{
										if (EmpiresVehicleAim::IsMgWeaponId(wIds[wi]))
										{
											mgClipAmmo = wAmmo[wi];
											break;
										}
									}
								}
							}
						}
						++v.mgEngTicks;
						v.mgEngLastTick = now;
						if (rcbot_empires_vehicle_shell_trace.GetBool())
						{
							static int s_mgSecUser = -1, s_mgSecTarget = -1, s_mgSecHp0 = 0, s_mgSecAmmo0 = -1, s_mgSecTicks = 0;
							static float s_mgSecStart = 0.0f, s_mgSecYawSum = 0.0f, s_mgSecYawMax = 0.0f;
							if (s_mgSecUser != userId || s_mgSecTarget != targetIndex || now - s_mgSecStart > 2.0f)
							{
								s_mgSecUser = userId;
								s_mgSecTarget = targetIndex;
								s_mgSecStart = now;
								s_mgSecHp0 = hp;
								s_mgSecAmmo0 = mgClipAmmo;
								s_mgSecTicks = 0;
								s_mgSecYawSum = 0.0f;
								s_mgSecYawMax = 0.0f;
							}
							const float absYaw = std::fabs(signedYawErr);
							s_mgSecYawSum += absYaw;
							if (absYaw > s_mgSecYawMax) s_mgSecYawMax = absYaw;
							++s_mgSecTicks;
							const float win = now - s_mgSecStart;
							if (win >= 1.0f || hp <= 0)
							{
								Vector tmins(0, 0, 0), tmaxs(0, 0, 0);
								if (target->GetCollideable() != nullptr)
									target->GetCollideable()->WorldSpaceSurroundingBounds(&tmins, &tmaxs);
								const Vector tOrig = CBotGlobals::entityOrigin(target);
								const int shots = (s_mgSecAmmo0 >= 0 && mgClipAmmo >= 0 && s_mgSecAmmo0 >= mgClipAmmo) ?
									(s_mgSecAmmo0 - mgClipAmmo) : -1;
								const int dmg = s_mgSecHp0 - hp;
								RCBOT_DIAG("[RCBOT2] vehicle mg-sec userid=%d ch=%d dist=%.0f dt=%.2f shots=%d hp=%d->%d dmg=%d hp_s=%.1f "
									"yerr_avg=%.1f yerr_max=%.1f yerr_s=%.1f wpitch=%.1f lpitch=%.1f hull_pr=(%.1f %.1f) vspd=%.0f "
									"tspd=%.0f tbox_h=%.0f aim_dz=%.1f eye_dz=%.1f heat=%.0f\n",
									userId, chassis, distance, win, shots, s_mgSecHp0, hp, dmg, win > 0.05f ? dmg / win : 0.0f,
									s_mgSecTicks > 0 ? s_mgSecYawSum / s_mgSecTicks : 0.0f, s_mgSecYawMax, signedYawErr,
									NormalizeYaw(worldView.x), NormalizeYaw(view.x), hullAngles.x, hullAngles.z, speed,
									Vector(v.aimVel.x, v.aimVel.y, 0.0f).Length(), tmaxs.z - tmins.z,
									aim.z - tOrig.z, eye.z - origin.z, heat);
								s_mgSecStart = now;
								s_mgSecHp0 = hp;
								s_mgSecAmmo0 = mgClipAmmo;
								s_mgSecTicks = 0;
								s_mgSecYawSum = 0.0f;
								s_mgSecYawMax = 0.0f;
							}
						}
					}
				}
				if (now >= v.nextAimLog - 3.5f && now >= v.nextAimCheck && rcbot_empires_vehicle_shell_trace.GetBool())
				{
					v.nextAimCheck = now + 1.5f;
					// Diagnosis: where turret and cannon really point (world collision angles; models
					// face local +Y, so the barrel's world yaw is entity yaw + 90).
					int turretIndex = -1;
					edict_t* turret = readEmpiresEntityHandleProperty(vehicle, "m_hTurret", 0, turretIndex) &&
						turretIndex > 0 && turretIndex < gpGlobals->maxEntities ? engine->PEntityOfEntIndex(turretIndex) : nullptr;
					edict_t* cannon = nullptr;
					float cannonDist = 400.0f;
					for (int i = gpGlobals->maxClients + 1; i < gpGlobals->maxEntities; ++i)
					{
						edict_t* e = engine->PEntityOfEntIndex(i);
						if (!IsClass(e, "emp_tank_cannon"))
							continue;
						const float d = (CBotGlobals::entityOrigin(e) - origin).Length();
						if (d < cannonDist)
						{
							cannonDist = d;
							cannon = e;
						}
					}
					const QAngle hullAng = vehicle->GetCollideable() != nullptr ? vehicle->GetCollideable()->GetCollisionAngles() : QAngle(0, 0, 0);
					const QAngle turretAng = turret != nullptr && turret->GetCollideable() != nullptr ?
						turret->GetCollideable()->GetCollisionAngles() : QAngle(0, 0, 0);
					const QAngle cannonAng = cannon != nullptr && cannon->GetCollideable() != nullptr ?
						cannon->GetCollideable()->GetCollisionAngles() : QAngle(0, 0, 0);
					const Vector cannonAt = cannon != nullptr ? CBotGlobals::entityOrigin(cannon) : Vector(0, 0, 0);
					float minYaw = -999.0f, maxYaw = -999.0f, wantYaw = -999.0f, turnSpeed = -999.0f;
					float wantPitch = -999.0f, minPitch = -999.0f, maxPitch = -999.0f;
					if (turret != nullptr)
					{
						ReadEntityFloat(turret, "m_fMinYaw", minYaw);
						ReadEntityFloat(turret, "m_fMaxYaw", maxYaw);
						ReadEntityFloat(turret, "m_fDesiredLocalYaw", wantYaw);
						ReadEntityFloat(turret, "m_fTurretTurnSpeed", turnSpeed);
					}
					if (cannon != nullptr)
					{
						ReadEntityFloat(cannon, "m_fDesiredLocalPitch", wantPitch);
						ReadEntityFloat(cannon, "m_fMinPitch", minPitch);
						ReadEntityFloat(cannon, "m_fMaxPitch", maxPitch);
					}
					const float* turretRot = turret != nullptr ? EntityFloatProp(turret, "m_angRotation") : nullptr;
					const float* hullRot = EntityFloatProp(vehicle, "m_angRotation");
					const float* cannonRot = cannon != nullptr ? EntityFloatProp(cannon, "m_angRotation") : nullptr;
					float targetTotal = -1.0f;
					ReadEntityFloat(target, "m_flTotalHealth", targetTotal);
					RCBOT_DIAG("[RCBOT2] vehicle target-health userid=%d target=%d m_iHealth=%d m_flTotalHealth=%.1f\n", userId,
						targetIndex, EmpiresVehicleReadInt(this, target, "m_iHealth", -1), targetTotal);
					RCBOT_DIAG("[RCBOT2] vehicle aim-props userid=%d turret=%d yaw_range=(%.1f %.1f) desired_local_yaw=%.1f "
						"turn=%.1f desired_local_pitch=%.1f pitch_range=(%.1f %.1f) rot_hull=(%.1f %.1f) rot_turret=(%.1f %.1f) "
						"rot_cannon=(%.1f %.1f)\n", userId, turretIndex, minYaw, maxYaw,
						wantYaw, turnSpeed, wantPitch, minPitch, maxPitch, hullRot ? hullRot[0] : -999.0f,
						hullRot ? hullRot[1] : -999.0f, turretRot ? turretRot[0] : -999.0f, turretRot ? turretRot[1] : -999.0f,
						cannonRot ? cannonRot[0] : -999.0f, cannonRot ? cannonRot[1] : -999.0f);
					RCBOT_DIAG("[RCBOT2] vehicle aim-check userid=%d want=(%.1f %.1f) hull=(%.1f %.1f %.1f) turret=(%.1f %.1f) "
						"cannon=%d(%.1f %.1f %.1f) cannon_at=(%.0f %.0f %.0f) eye_z=%.0f aim=(%.0f %.0f %.0f) aimed_for=%.1f\n",
						userId, worldView.x, worldView.y, hullAng.x, NormalizeYaw(hullAng.y + 90.0f), hullAng.z, turretAng.x,
						NormalizeYaw(turretAng.y + 90.0f), cannon != nullptr ? engine->IndexOfEdict(cannon) : -1, cannonAng.x,
						NormalizeYaw(cannonAng.y + 90.0f), cannonAng.z, cannonAt.x, cannonAt.y, cannonAt.z, eye.z,
						aim.x, aim.y, aim.z, now - v.aimSince);
				}
				if (now >= v.nextAimLog)
				{
					v.nextAimLog = now + 5.0f;
					RCBOT_DIAG("[RCBOT2] vehicle fire userid=%d target=%d infantry=%d dist=%.0f buttons=%s%s why=%s lead=%.0f ballistics=%d weapons=%s\n",
						userId, targetIndex, infantry ? 1 : 0, distance,
						(buttons & IN_ATTACK) ? "a1" : "", (buttons & IN_ATTACK2) ? "a2" : "", why,
						(aim - centre).Length(), static_cast<int>(ballistics.mode), v.weaponsKnown ? v.weaponSummary : "unknown");
				}
			}
		}
	}

	const float clipLook = (std::min)(goalDistance, lookAhead);
	const bool clipAhead = forward > 0.0f && RCBotNavMesh::VehicleClipBlocks(origin,
		origin + YawDir(chosen) * clipLook, v.width);
	if (clipBlockedRoute || clipAhead)
	{
		forward = side = 0.0f;
		buttons |= 0x08000000;
	}
	v.forward = forward;
	v.side = side;
	v.buttons = buttons;
	v.view = view;

	if (now >= v.nextLog)
	{
		v.nextLog = now + 2.0f;
		float targetHealth = -1.0f;
		if (target != nullptr)
		{
			int hp = -1;
			if (!ReadEntityFloat(target, "m_flTotalHealth", targetHealth))
				targetHealth = readEmpiresEntityIntProperty(target, "m_iHealth", hp) ? float(hp) : -1.0f;
		}
		RCBOT_DIAG("[RCBOT2] vehicle drive userid=%d veh=%d chassis=%d pos=(%.0f %.0f %.0f) heading=%.0f speed=%.0f "
			"goal=(%.0f %.0f) dist=%.0f desired=%.0f chosen=%.0f err=%.0f fwd=%.0f side=%.0f safe=%d/%d "
			"target=%d thp=%.0f btn=0x%x ram=%d billnye=%d route=%zu/%zu curve_cap=%.0f tick=%d local=%d maneuver_apply=%d maneuver_speed=%.0f blocker=%d\n",
			userId, v.vehicleIndex, chassis, origin.x, origin.y, origin.z, heading, speed,
			driveTo.x, driveTo.y, goalDistance, desired, chosen, err, forward, side,
			found ? 1 : 0, headingSafe ? 1 : 0,
			target != nullptr ? engine->IndexOfEdict(target) : -1, targetHealth, buttons,
			ram ? 1 : 0, v.billNye ? 1 : 0, onRoute ? v.routeIndex : 0, v.route.size(),
			curveSpeed, gpGlobals->tickcount, localManeuvers ? 1 : 0,
			applyManeuver ? 1 : 0, applyManeuver ? v.maneuverSpeed : 0.0f, localManeuvers ? v.maneuverBlocker : -1);
	}
	if (v.billNye && targetIsVehicle && targetAt.z < origin.z - 400.0f && now >= v.nextAction)
	{
		v.nextAction = now + 5.0f;
		RCBOT_DIAG("[RCBOT2] billnye pushed-off userid=%d target=%d z=%.0f\n", userId,
			engine->IndexOfEdict(target), targetAt.z);
	}
}

bool CBotEmpires::startEmpiresCommanderDrive(edict_t* cv, const Vector& goal)
{
	if (cv == nullptr || cv->IsFree() || engine == nullptr || gpGlobals == nullptr ||
		EmpiresVehiclePlayerVehicle(this, m_pEdict) != engine->IndexOfEdict(cv))
		return false;
	resetEmpiresVehicle("commander-drive", 0.0f);
	EmpiresVehicleBrain& v = m_vehicle;
	v.vehicle = cv;
	v.vehicleIndex = engine->IndexOfEdict(cv);
	v.commanderCV = true;
	v.cvGoal = goal;
	v.lastVehicleOrigin = CBotGlobals::entityOrigin(cv);
	v.lastSampleTime = gpGlobals->curtime;
	v.stage = EmpiresVehicleStage::DRIVING;
	RCBOT_DIAG("[RCBOT2] cv-drive start userid=%d cv=%d from=(%.0f %.0f %.0f) goal=(%.0f %.0f %.0f) tick=%d\n",
		engine->GetPlayerUserId(m_pEdict), v.vehicleIndex, v.lastVehicleOrigin.x, v.lastVehicleOrigin.y,
		v.lastVehicleOrigin.z, goal.x, goal.y, goal.z, gpGlobals->tickcount);
	return true;
}

bool EmpiresEngibotWithinLeash(const edict_t* player, const Vector& at); // bot_empires_engibot.cpp

// A vehicle's repair call may take this engineer: not while the commander's order stands (the
// assist owned the bot for up to 60 s and orders were ignored, human test 15.11., run
// 20261115-125702), and an EngiBot only inside its base leash.
bool CBotEmpires::empiresRepairCallAllowed(edict_t* hurt)
{
	if (hurt == nullptr || m_pEdict == nullptr)
		return false;
	if (commanderOrderHoldsFrom(hurt))
		return false;
	return !EmpiresIsEngibot(m_pEdict) || EmpiresEngibotWithinLeash(m_pEdict, CBotGlobals::entityOrigin(hurt));
}

bool CBotEmpires::empiresVehicleRepairClaimed() const
{
	if (gpGlobals == nullptr || engine == nullptr || m_pEdict == nullptr)
		return false;
	const int team = CClassInterface::getTeam(m_pEdict);
	if (team < 0 || team >= 4)
		return false;
	const RepairRequest& r = s_repairRequest[team];
	return r.engineerUserId == engine->GetPlayerUserId(m_pEdict) && gpGlobals->curtime <= r.claimUntil &&
		RepairRequestVehicle(team) != nullptr;
}

edict_t* CBotEmpires::findEmpiresBuddyRepairVehicle(int& worstSide, bool& armorDamaged, int& driverUserId)
{
	worstSide = -1;
	armorDamaged = false;
	driverUserId = -1;
	const int mode = rcbot_empires_buddy_repair.GetInt();
	if (mode <= 0 || gpGlobals == nullptr || engine == nullptr || playerinfomanager == nullptr)
		return nullptr;
	const int team = getTeam();
	const Vector origin = getOrigin();
	edict_t* best = nullptr;
	float bestDist = 1200.0f;
	for (int i = gpGlobals->maxClients + 1; i < gpGlobals->maxEntities; ++i)
	{
		edict_t* e = engine->PEntityOfEntIndex(i);
		if (e == nullptr || e->IsFree() || !IsClass(e, "emp_vehicle") ||
			EmpiresVehicleReadInt(this, e, "m_iTeamNum", -1) != team ||
			IsCommanderChassis(EmpiresVehicleReadInt(this, e, "m_iChassis", -1)))
			continue;
		const float d = Dist2D(origin, CBotGlobals::entityOrigin(e));
		if (d >= bestDist)
			continue;
		const int uid = engine->GetPlayerUserId(m_pEdict);
		auto skip = [&](const char* why) {
			RCBOT_DIAG("[RCBOT2] buddy-repair skip userid=%d veh=%d dist=%.0f reason=%s\n", uid, i, d, why);
		};
		if (!empiresRepairCallAllowed(e))
		{
			skip("order");
			continue;
		}
		// Driver seat: a human, or empty (parked); bots call for repair themselves.
		int driver = -1;
		int driverUid = -1;
		if (readEmpiresEntityHandleProperty(e, "m_hPassengers", 0, driver) && driver > 0 && driver <= gpGlobals->maxClients)
		{
			edict_t* p = engine->PEntityOfEntIndex(driver);
			IPlayerInfo* info = p != nullptr ? playerinfomanager->GetPlayerInfo(p) : nullptr;
			if (info == nullptr || (!EmpiresIsHuman(p) && mode < 2))
			{
				continue;
			}
			driverUid = engine->GetPlayerUserId(p);
		}
		int side = -1;
		bool armor = false;
		if (!VehicleNeedsBaseRepair(this, e, 60.0f, side, armor))
		{
			continue;
		}
		// Worth walking over for: hull below 90 % or an armor side below 85 %.
		float hp = 0.0f, maxHp = 0.0f;
		const bool hullLow = ReadVehicleHealth(e, hp) && ReadVehicleMaxHealth(e, maxHp) && hp < maxHp * 0.9f;
		const int armorMax = EmpiresVehicleReadInt(this, e, "m_iMaxArmorHealth", 0);
		const bool armorLow = armor && side >= 0 && armorMax > 0 && ReadArmorSide(e, side) < armorMax * 0.85f;
		if (!hullLow && !armorLow)
		{
			continue;
		}
		best = e;
		bestDist = d;
		worstSide = side;
		armorDamaged = armor;
		driverUserId = driverUid;
	}
	return best;
}

bool CBotEmpires::handleEmpiresVehicleRepairRequest()
{
	if (gpGlobals == nullptr || engine == nullptr || !isEmpiresEngineerClass())
		return false;
	const int team = getTeam();
	// TODO 8.3: no call standing -> look for a damaged human or parked vehicle nearby and help unasked
	// (EngiBots have their own base scan). Uses the bot repair call, so the walk and REPAIR_ASSIST stay the same.
	// A standing bot call gives way to a damaged human-driven vehicle nearby (humans first).
	const bool botCallStands = team >= 0 && team < 4 && RepairRequestVehicle(team) != nullptr &&
		!s_repairRequest[team].human && rcbot_empires_humans_first.GetBool();
	if ((RepairRequestVehicle(team) == nullptr || botCallStands) && !EmpiresIsEngibot(m_pEdict) &&
		gpGlobals->curtime >= m_flBuddyRepairScan && team >= 0 && team < 4)
	{
		m_flBuddyRepairScan = gpGlobals->curtime + 2.0f;
		int side = -1, driverUid = -1;
		bool armorDamaged = false;
		edict_t* buddy = findEmpiresBuddyRepairVehicle(side, armorDamaged, driverUid);
		const bool humanDriver = buddy != nullptr && EmpiresIsHumanUserId(driverUid);
		if (buddy != nullptr && botCallStands && (!humanDriver || engine->IndexOfEdict(buddy) == s_repairRequest[team].vehicle))
			buddy = nullptr;
		if (buddy != nullptr)
		{
			RepairRequest& r = s_repairRequest[team];
			if (botCallStands)
				RCBOT_DIAG("[RCBOT2] buddy-repair human-first userid=%d bot_veh=%d human_veh=%d\n",
					engine->GetPlayerUserId(m_pEdict), r.vehicle, engine->IndexOfEdict(buddy));
			r = RepairRequest();
			r.vehicle = engine->IndexOfEdict(buddy);
			r.driverUserId = driverUid;
			r.origin = CBotGlobals::entityOrigin(buddy);
			r.until = gpGlobals->curtime + 20.0f;
			r.side = side;
			r.armorDamaged = armorDamaged;
			r.baseScan = true; // repair until full, like the EngiBot scan
			r.human = humanDriver;
			RCBOT_DIAG("[RCBOT2] buddy-repair start userid=%d veh=%d driver=%d dist=%.0f side=%d armor=%d\n",
				engine->GetPlayerUserId(m_pEdict), r.vehicle, driverUid, Dist2D(getOrigin(), r.origin), side,
				armorDamaged ? 1 : 0);
		}
	}
	edict_t* hurt = RepairRequestVehicle(team);
	if (hurt == nullptr || !empiresRepairCallAllowed(hurt))
		return false;
	const float now = gpGlobals->curtime;
	const int userId = engine->GetPlayerUserId(m_pEdict);
	RepairRequest& r = s_repairRequest[team];
	if (r.engineerUserId != userId && now <= r.claimUntil)
		return false; // another engineer has it
	const Vector at = CBotGlobals::entityOrigin(hurt);
	if (Dist2D(getOrigin(), at) > 16000.0f)
		return false;
	r.engineerUserId = userId;
	r.claimUntil = now + 10.0f;
	// Re-issue only when idle, the vehicle moved, or after a long while:
	// every new request restarts the route (run 220710: the engineer swung
	// between 7800 and 8700 away with a request every 5 s).
	const bool idle = m_pSchedules == nullptr || m_pSchedules->isEmpty();
	if ((idle && now >= m_vehicle.nextAssistNav) || Dist2D(m_vehicle.assistNavGoal, at) > 400.0f ||
		now >= m_vehicle.nextAssistNav + 25.0f)
	{
		m_vehicle.nextAssistNav = now + 5.0f;
		m_vehicle.assistNavGoal = at;
		// Stand next to the hull, on our side.
		Vector spot = NearestOnVehicle(hurt, getOrigin());
		Vector away = getOrigin() - spot;
		away.z = 0.0f;
		if (away.Length() > 1.0f)
			spot += away.Normalized() * 60.0f;
		Vector onMesh;
		if (RCBotNavMesh::ClosestPointOnMesh(spot, 200.0f, onMesh))
			spot = onMesh;
		if (m_bEmpiresNavGoalActive)
			endEmpiresNavmeshGoal("drop", "vehicle-repair", 0.0f);
		if (!requestNavigation(spot, RCBotNavigation::NavigationPolicy::NAVMESH_ONLY, 0))
			return false;
		RCBOT_DIAG("[RCBOT2] vehicle repair-walk userid=%d veh=%d to=(%.0f %.0f %.0f) dist=%.0f\n", userId,
			engine->IndexOfEdict(hurt), spot.x, spot.y, spot.z, Dist2D(getOrigin(), at));
	}
	return true;
}

void CBotEmpires::endEmpiresTankEscort(const char* reason)
{
	if (m_iEscortVehicle < 0)
		return;
	const int team = getTeam();
	const int userId = engine != nullptr ? engine->GetPlayerUserId(m_pEdict) : -1;
	if (team >= 0 && team < 4 && s_escortClaim[team].engineerUserId == userId)
		s_escortClaim[team] = EscortClaim();
	RCBOT_DIAG("[RCBOT2] tank-escort end userid=%d veh=%d reason=%s\n", userId, m_iEscortVehicle, reason);
	m_iEscortVehicle = -1;
}

// TODO 8.4: one free engineer per team walks with an own driven tank (not the CV, not an APC) and stays
// ~350 HU behind it; when the tank stands damaged it gets the normal repair call (REPAIR_ASSIST walk
// and repair stay the same). Sits below turrets/walls in the engineer chain, above capture/squad/roam.
bool CBotEmpires::handleEmpiresTankEscort()
{
	if (!rcbot_empires_tank_escort.GetBool() || gpGlobals == nullptr || engine == nullptr || m_pEdict == nullptr ||
		!isEmpiresEngineerClass() || EmpiresIsEngibot(m_pEdict))
	{
		endEmpiresTankEscort("off");
		return false;
	}
	const int team = getTeam();
	if (team >= 2 && team <= 3 && m_iEscortVehicle > 0 && m_vehicle.stage == EmpiresVehicleStage::REPAIR_ASSIST &&
		m_vehicle.assistVehicle.get() != nullptr && engine->IndexOfEdict(m_vehicle.assistVehicle.get()) == m_iEscortVehicle)
	{
		s_escortClaim[team].until = gpGlobals->curtime + 10.0f; // repairing our tank: still its escort
		return false;
	}
	if (team < 2 || team > 3 || EmpiresVehiclePlayerVehicle(this, m_pEdict) >= 0 || m_vehicle.stage != EmpiresVehicleStage::NONE)
	{
		endEmpiresTankEscort("busy");
		return false;
	}
	const float now = gpGlobals->curtime;
	const int userId = engine->GetPlayerUserId(m_pEdict);
	const Vector origin = getOrigin();
	EscortClaim& claim = s_escortClaim[team];
	auto tankOk = [&](int index, float maxDist) -> edict_t* {
		edict_t* e = index > 0 ? engine->PEntityOfEntIndex(index) : nullptr;
		if (e == nullptr || e->IsFree() || !IsClass(e, "emp_vehicle") || !EmpiresVehicleAlive(this, e) ||
			EmpiresVehicleReadInt(this, e, "m_iTeamNum", -1) != team)
			return nullptr;
		const int chassis = EmpiresVehicleReadInt(this, e, "m_iChassis", -1);
		if (IsCommanderChassis(chassis) || IsApcChassis(chassis))
			return nullptr;
		int driver = -1;
		if (!readEmpiresEntityHandleProperty(e, "m_hPassengers", 0, driver) || driver <= 0 || driver > gpGlobals->maxClients)
			return nullptr; // parked/empty: buddy repair (8.3) covers it
		return Dist2D(origin, CBotGlobals::entityOrigin(e)) <= maxDist ? e : nullptr;
	};
	edict_t* tank = m_iEscortVehicle > 0 ? tankOk(m_iEscortVehicle, 9000.0f) : nullptr;
	if (m_iEscortVehicle > 0 && tank == nullptr)
		endEmpiresTankEscort("tank-gone");
	if (tank == nullptr)
	{
		if (now < m_flEscortNextScan || now < m_flEscortCooldownUntil)
			return false;
		m_flEscortNextScan = now + 3.0f;
		if (claim.engineerUserId >= 0 && claim.engineerUserId != userId && now < claim.until)
			return false; // another engineer escorts already
		float bestDist = 3000.0f;
		for (int i = gpGlobals->maxClients + 1; i < gpGlobals->maxEntities; ++i)
		{
			if (i == m_iEscortRejected && now < m_flEscortCooldownUntil + 60.0f)
				continue;
			edict_t* e = tankOk(i, bestDist);
			if (e == nullptr)
				continue;
			bestDist = Dist2D(origin, CBotGlobals::entityOrigin(e));
			tank = e;
		}
		if (tank == nullptr)
			return false;
		m_iEscortVehicle = engine->IndexOfEdict(tank);
		m_vEscortTankLast = CBotGlobals::entityOrigin(tank);
		m_vEscortBehind = origin - m_vEscortTankLast;
		m_vEscortBehind.z = 0.0f;
		m_flEscortNextNav = 0.0f;
		m_flEscortProgressAt = now;
		m_flEscortBestDist = 1.0e9f;
		m_vEscortNavGoal = Vector(0, 0, 0);
		RCBOT_DIAG("[RCBOT2] tank-escort start userid=%d veh=%d dist=%.0f\n", userId, m_iEscortVehicle, bestDist);
	}
	claim.engineerUserId = userId;
	claim.vehicle = m_iEscortVehicle;
	claim.until = now + 10.0f;

	const Vector at = CBotGlobals::entityOrigin(tank);
	Vector moved = m_vEscortTankLast - at;
	moved.z = 0.0f;
	if (moved.Length() > 150.0f)
	{
		m_vEscortBehind = moved; // opposite of the tank's travel
		m_vEscortTankLast = at;
	}
	Vector behind = m_vEscortBehind;
	if (behind.Length() < 1.0f)
		behind = Vector(1, 0, 0);
	behind = behind.Normalized();
	const float toTank = Dist2D(origin, at);

	// Standing and damaged: hand it to the normal repair call (this engineer is nearest anyway).
	if (toTank < 900.0f && RepairRequestVehicle(team) == nullptr)
	{
		int side = -1;
		bool armorDamaged = false;
		float hp = 0.0f, maxHp = 0.0f;
		const bool hullLow = ReadVehicleHealth(tank, hp) && ReadVehicleMaxHealth(tank, maxHp) && hp < maxHp * 0.95f;
		const int armorMax = EmpiresVehicleReadInt(this, tank, "m_iMaxArmorHealth", 0);
		if (VehicleNeedsBaseRepair(this, tank, 60.0f, side, armorDamaged) &&
			(hullLow || (armorDamaged && side >= 0 && armorMax > 0 && ReadArmorSide(tank, side) < armorMax * 0.9f)))
		{
			RepairRequest& r = s_repairRequest[team];
			r = RepairRequest();
			r.vehicle = m_iEscortVehicle;
			r.origin = at;
			r.until = now + 40.0f; // the tank first waits for us (up to 15 s), then holds for the repair
			r.side = side;
			r.armorDamaged = armorDamaged;
			r.baseScan = true;
			r.engineerUserId = userId;
			r.claimUntil = now + 10.0f;
			RCBOT_DIAG("[RCBOT2] tank-escort repair userid=%d veh=%d dist=%.0f side=%d armor=%d\n", userId,
				m_iEscortVehicle, toTank, side, armorDamaged ? 1 : 0);
			return false; // the repair handler (earlier in the chain) takes it from the next tick
		}
	}

	Vector spot = at + behind * 350.0f;
	Vector onMesh;
	if (RCBotNavMesh::ClosestPointOnMesh(spot, 250.0f, onMesh))
		spot = onMesh;
	const float left = Dist2D(origin, spot);
	if (Dist2D(m_vEscortNavGoal, spot) > 300.0f)
	{
		m_flEscortBestDist = 1.0e9f; // the tank moved on: a new walk, not a stall
		m_flEscortProgressAt = now;
	}
	if (left < 200.0f || toTank < 500.0f)
	{
		m_flEscortProgressAt = now;
		m_flEscortBestDist = left;
		return true; // in place: hold (combat runs before this chain)
	}
	if (left + 100.0f < m_flEscortBestDist)
	{
		m_flEscortBestDist = left;
		m_flEscortProgressAt = now;
	}
	if (now - m_flEscortProgressAt > 25.0f)
	{
		RCBOT_DIAG("[RCBOT2] stall-escalate who=tank-escort stage=1 reason=no-progress veh=%d left=%.0f\n",
			m_iEscortVehicle, left);
		m_iEscortRejected = m_iEscortVehicle;
		m_flEscortCooldownUntil = now + 30.0f;
		endEmpiresTankEscort("no-progress");
		return false;
	}
	const bool idle = m_pSchedules == nullptr || m_pSchedules->isEmpty();
	if ((idle && now >= m_flEscortNextNav) || (Dist2D(m_vEscortNavGoal, spot) > 300.0f && now >= m_flEscortNextNav))
	{
		m_flEscortNextNav = now + 2.0f;
		m_vEscortNavGoal = spot;
		if (m_bEmpiresNavGoalActive)
			endEmpiresNavmeshGoal("drop", "tank-escort", 0.0f);
		if (!requestNavigation(spot, RCBotNavigation::NavigationPolicy::NAVMESH_ONLY, 0))
		{
			m_iEscortRejected = m_iEscortVehicle;
			m_flEscortCooldownUntil = now + 30.0f;
			endEmpiresTankEscort("no-path");
			return false;
		}
		RCBOT_DIAG("[RCBOT2] tank-escort follow userid=%d veh=%d to=(%.0f %.0f %.0f) left=%.0f tank_dist=%.0f\n", userId,
			m_iEscortVehicle, spot.x, spot.y, spot.z, left, toTank);
	}
	return true;
}

namespace
{
struct WreckClaim
{
	int engineerUserId = -1;
	float until = 0.0f;
};
WreckClaim s_wreckClaim[MAX_EDICTS];

bool FactoryBuildAttachmentPos(edict_t* factory, Vector& build)
{
	if (factory == nullptr || factory->IsFree() || factory->GetCollideable() == nullptr)
		return false;
	const bool nf = CClassInterface::getTeam(factory) == 2;
	matrix3x4_t toWorld;
	AngleMatrix(factory->GetCollideable()->GetCollisionAngles(), factory->GetCollideable()->GetCollisionOrigin(), toWorld);
	VectorTransform(nf ? Vector(172.5f, 0.0f, 172.5f) : Vector(0.0f, 0.0f, 200.0f), toWorld, build);
	return true;
}
} // namespace

// 5.8a / 3.17: Engineers deconstruct destroyed vehicle carcasses (m_iCarcassHealth > 0, !alive)
// blocking an own vehicle factory bay or sitting in base, using the Engineer kit PrimaryAttack
// (server_srv.so 0x3d0d61..0x3d1096 removes the carcass and refunds resources for any team's wreck).
bool CBotEmpires::handleEmpiresWreckDeconstruct()
{
	m_bWreckClearAttacking = false;
	if (gpGlobals == nullptr || engine == nullptr || !isEmpiresEngineerClass() ||
		m_iReviveTarget != 0 || m_iBuildAssignStage > 0 || m_iAmmoCrateStage != 0 ||
		m_deploy.stage >= 2 || m_iTurretRefinery != 0 || m_iTurretBuilding != 0 ||
		m_bTurretBuildArmed || m_iTurretUpgrade != 0 || commanderDutyBlocksSideTasks() ||
		m_vehicle.stage != EmpiresVehicleStage::NONE)
	{
		m_iWreckClearTarget = 0;
		return false;
	}
	const float now = gpGlobals->curtime;
	const int team = getTeam();
	const int userId = engine->GetPlayerUserId(m_pEdict);
	if (team != 2 && team != 3)
		return false;

	auto stopWreck = [&](const char* reason, float cooldown)
	{
		if (m_iWreckClearTarget > 0 && m_iWreckClearTarget < MAX_EDICTS)
			s_wreckClaim[m_iWreckClearTarget] = {userId, now + cooldown};
		RCBOT_DIAG("[RCBOT2] wreck-clear end userid=%d veh=%d bay=%d reason=%s hp=%d\n",
			userId, m_iWreckClearTarget, m_bWreckClearBay ? 1 : 0, reason, m_iWreckClearLastHp);
		m_iWreckClearTarget = 0;
		m_iWreckClearLastHp = -1;
		m_bWreckClearBay = false;
		m_bWreckClearAttacking = false;
		m_bWreckClearProgressLogged = false;
		m_flWreckClearNextSearch = now + 2.0f;
		cancelNavigationAttempt("wreck-clear-end");
		setLookAtTask(LOOK_NONE);
		return false;
	};

	if (m_iWreckClearTarget == 0)
	{
		if (now < m_flWreckClearNextSearch)
			return false;
		m_flWreckClearNextSearch = now + 2.0f;

		edict_t* factory = nullptr;
		Vector factoryOrigin, factoryBuild;
		int factoryTeam = -1;
		const bool hasFactory = findEmpiresVehicleFactory(team, factory, factoryOrigin, factoryTeam) &&
			FactoryBuildAttachmentPos(factory, factoryBuild);

		const char* cvClass = team == 2 ? "emp_nf_commander" : "emp_imp_commander";
		Vector cvPos;
		bool hasCV = false;
		for (int i = gpGlobals->maxClients + 1; i < gpGlobals->maxEntities && !hasCV; ++i)
		{
			edict_t* e = engine->PEntityOfEntIndex(i);
			if (IsClass(e, cvClass))
			{
				cvPos = CBotGlobals::entityOrigin(e);
				hasCV = true;
			}
		}

		int bestIdx = 0;
		int bestHp = 0;
		bool bestBay = false;
		float bestScore = 1.0e9f;
		float bestDist = 0.0f;
		for (int i = gpGlobals->maxClients + 1; i < gpGlobals->maxEntities; ++i)
		{
			edict_t* e = engine->PEntityOfEntIndex(i);
			if (!IsClass(e, "emp_vehicle"))
				continue;
			if (now < s_wreckClaim[i].until)
				continue;
			const bool alive = EmpiresVehicleAlive(this, e);
			int carcassHp = 0;
			if (alive || !readEmpiresEntityIntProperty(e, "m_iCarcassHealth", carcassHp) || carcassHp <= 0)
				continue;
			const Vector at = CBotGlobals::entityOrigin(e);
			const float distToEng = Dist2D(getOrigin(), at);
			const float distToBuild = hasFactory ? Dist2D(at, factoryBuild) : 1.0e9f;
			const float distToCV = hasCV ? Dist2D(at, cvPos) : 1.0e9f;
			const EmpiresWreckClear::CarcassEval ev = EmpiresWreckClear::EvaluateCarcass(
				alive, carcassHp, distToEng, hasFactory, distToBuild, hasCV, distToCV);
			if (!ev.eligible || ev.score >= bestScore)
				continue;
			bestScore = ev.score;
			bestIdx = i;
			bestHp = carcassHp;
			bestBay = ev.blocksFactory;
			bestDist = distToEng;
		}
		if (bestIdx == 0)
			return false;
		m_iWreckClearTarget = bestIdx;
		m_iWreckClearLastHp = bestHp;
		m_bWreckClearBay = bestBay;
		m_bWreckClearProgressLogged = false;
		m_flWreckClearNextNav = 0.0f;
		m_flWreckClearProgressAt = now;
		m_flWreckClearBestDist = bestDist;
		s_wreckClaim[bestIdx] = {userId, now + 4.0f};
		RCBOT_DIAG("[RCBOT2] wreck-clear start userid=%d veh=%d bay=%d hp=%d dist=%.0f\n",
			userId, bestIdx, bestBay ? 1 : 0, bestHp, bestDist);
	}

	edict_t* carcass = m_iWreckClearTarget > 0 && m_iWreckClearTarget < MAX_EDICTS ?
		engine->PEntityOfEntIndex(m_iWreckClearTarget) : nullptr;
	int carcassHp = 0;
	if (!IsClass(carcass, "emp_vehicle") || EmpiresVehicleAlive(this, carcass) ||
		!readEmpiresEntityIntProperty(carcass, "m_iCarcassHealth", carcassHp) || carcassHp <= 0)
	{
		RCBOT_DIAG("[RCBOT2] wreck-clear done userid=%d veh=%d bay=%d reason=removed\n",
			userId, m_iWreckClearTarget, m_bWreckClearBay ? 1 : 0);
		if (m_iWreckClearTarget > 0 && m_iWreckClearTarget < MAX_EDICTS)
			s_wreckClaim[m_iWreckClearTarget] = WreckClaim();
		m_iWreckClearTarget = 0;
		m_iWreckClearLastHp = -1;
		m_bWreckClearBay = false;
		m_flWreckClearNextSearch = now + 2.0f;
		cancelNavigationAttempt("wreck-clear-done");
		setLookAtTask(LOOK_NONE);
		return false;
	}
	s_wreckClaim[m_iWreckClearTarget] = {userId, now + 4.0f};

	const Vector eye = getEyePosition();
	Vector kitAim = CBotGlobals::worldCenter(carcass);
	const bool haveKitAim = findEmpiresKitAim(carcass, eye, kitAim, false);
	const float reach = (NearestOnVehicle(carcass, eye) - eye).Length();
	if (!haveKitAim && reach > 55.0f)
	{
		const float dist = Dist2D(getOrigin(), CBotGlobals::entityOrigin(carcass));
		if (dist < m_flWreckClearBestDist - 40.0f)
		{
			m_flWreckClearBestDist = dist;
			m_flWreckClearProgressAt = now;
		}
		else if (now - m_flWreckClearProgressAt > 15.0f)
		{
			RCBOT_DIAG("[RCBOT2] stall-escalate who=wreck-clear stage=2 reason=walk-stuck userid=%d veh=%d dist=%.0f reach=%.0f\n",
				userId, m_iWreckClearTarget, dist, reach);
			return stopWreck("walk-stuck", 45.0f);
		}
		const bool idle = m_pSchedules == nullptr || m_pSchedules->isEmpty();
		if (now >= m_flWreckClearNextNav && (idle || now >= m_flWreckClearNextNav + 8.0f))
		{
			m_flWreckClearNextNav = now + 3.0f;
			Vector spot = NearestOnVehicle(carcass, getOrigin());
			Vector away = getOrigin() - spot;
			away.z = 0.0f;
			if (away.Length() > 1.0f)
				spot += away.Normalized() * 35.0f;
			Vector onMesh;
			if (RCBotNavMesh::IsLoaded() && RCBotNavMesh::ClosestPointOnMesh(spot, 160.0f, onMesh))
				spot = onMesh;
			if (m_bEmpiresNavGoalActive)
				endEmpiresNavmeshGoal("drop", "wreck-clear", 0.0f);
			if (dist < 220.0f || !requestNavigation(spot, RCBotNavigation::NavigationPolicy::NAVMESH_ONLY, 0))
				setMoveTo(spot);
		}
		if (dist < 220.0f)
		{
			Vector spot = NearestOnVehicle(carcass, getOrigin());
			Vector away = getOrigin() - spot;
			away.z = 0.0f;
			if (away.Length() > 1.0f)
				spot += away.Normalized() * 35.0f;
			setMoveTo(spot);
		}
		return true;
	}

	// Within kit reach: clear build item, select engineer kit, aim at verified MASK_SHOT hit on carcass.
	cancelNavigationAttempt("wreck-clear-work");
	if (m_pSchedules != nullptr && !m_pSchedules->isEmpty())
		m_pSchedules->freeMemory();
	stopMoving();

	const char* kit = team == 2 ? "weapon_emp_nf_engineer_kit" : "weapon_emp_imp_engineer_kit";
	const edict_t* curWep = CClassInterface::getCurrentWeapon(m_pEdict);
	const bool kitActive = curWep != nullptr && !curWep->IsFree() && curWep->GetClassName() != nullptr &&
		std::strcmp(curWep->GetClassName(), kit) == 0;
	if (!kitActive)
		selectWeaponName(kit);
	int buildItem = 0;
	if (readEmpiresDebugIntProperty(m_pEdict, "m_iEngineerBuildItem", buildItem) && buildItem != 0 && helpers != nullptr)
		helpers->ClientCommand(m_pEdict, "engSelect 0\n");

	if (carcassHp < m_iWreckClearLastHp)
	{
		if (!m_bWreckClearProgressLogged)
		{
			m_bWreckClearProgressLogged = true;
			RCBOT_DIAG("[RCBOT2] wreck-clear progress userid=%d veh=%d hp=%d->%d\n",
				userId, m_iWreckClearTarget, m_iWreckClearLastHp, carcassHp);
		}
		m_iWreckClearLastHp = carcassHp;
		m_flWreckClearProgressAt = now;
	}
	else if (now - m_flWreckClearProgressAt > 10.0f)
	{
		RCBOT_DIAG("[RCBOT2] stall-escalate who=wreck-clear stage=2 reason=no-progress userid=%d veh=%d hp=%d reach=%.0f aim=%d kit=%d item=%d\n",
			userId, m_iWreckClearTarget, carcassHp, reach, haveKitAim ? 1 : 0, kitActive ? 1 : 0, buildItem);
		return stopWreck("no-progress", 45.0f);
	}

	setLookVector(kitAim);
	setLookAtTask(LOOK_VECTOR, 0.25f);
	if (kitActive && buildItem == 0)
	{
		primaryAttack(true, 0.35f);
		m_bWreckClearAttacking = true;
	}
	return true;
}

// 5.8a: If a live enemy vehicle is parked inside our own vehicle factory bay, send a nearby
// Grenadier toward the bay so handleEmpiresRocketVehicle gets sightline and destroys it.
bool CBotEmpires::handleEmpiresFactoryBayEnemy()
{
	if (gpGlobals == nullptr || engine == nullptr || m_vehicle.stage != EmpiresVehicleStage::NONE ||
		commanderDutyBlocksSideTasks())
		return false;
	int classId = -1;
	bool classDesired = false, classKnown = false;
	const char* prop = nullptr;
	if (!readEmpiresClass(m_pEdict, classId, classDesired, classKnown, prop) || classId != EMP_CLASS_GRENADIER)
		return false;
	const int team = getTeam();
	if (team != 2 && team != 3)
		return false;

	edict_t* factory = nullptr;
	Vector factoryOrigin, factoryBuild;
	int factoryTeam = -1;
	if (!findEmpiresVehicleFactory(team, factory, factoryOrigin, factoryTeam) ||
		!FactoryBuildAttachmentPos(factory, factoryBuild))
		return false;

	edict_t* blocker = nullptr;
	float bestDist = 1.0e9f;
	for (int i = gpGlobals->maxClients + 1; i < gpGlobals->maxEntities; ++i)
	{
		edict_t* e = engine->PEntityOfEntIndex(i);
		if (!IsClass(e, "emp_vehicle"))
			continue;
		const bool alive = EmpiresVehicleAlive(this, e);
		const int vehTeam = EmpiresVehicleReadInt(this, e, "m_iTeamNum", -1);
		const Vector at = CBotGlobals::entityOrigin(e);
		const float distToBay = Dist2D(at, factoryBuild);
		const float distToMe = Dist2D(getOrigin(), at);
		if (!EmpiresWreckClear::IsEnemyFactoryBlocker(alive, vehTeam, team, true, distToBay, distToMe))
			continue;
		if (distToMe < bestDist)
		{
			bestDist = distToMe;
			blocker = e;
		}
	}
	if (blocker == nullptr)
		return false;
	const float now = gpGlobals->curtime;
	if (now < m_flBayEnemyNextNav)
		return true;
	m_flBayEnemyNextNav = now + 4.0f;
	const Vector targetPos = CBotGlobals::entityOrigin(blocker);
	Vector stand = targetPos;
	Vector away = getOrigin() - targetPos;
	away.z = 0.0f;
	if (away.Length() > 1.0f)
		stand = targetPos + away.Normalized() * 500.0f;
	Vector onMesh;
	if (RCBotNavMesh::IsLoaded() && RCBotNavMesh::ClosestPointOnMesh(stand, 250.0f, onMesh))
		stand = onMesh;
	if (m_bEmpiresNavGoalActive)
		endEmpiresNavmeshGoal("drop", "bay-enemy-clear", 0.0f);
	if (!requestNavigation(stand, RCBotNavigation::NavigationPolicy::NAVMESH_ONLY, 0))
		return false;
	RCBOT_DIAG("[RCBOT2] bay-enemy-clear userid=%d veh=%d dist=%.0f\n",
		engine->GetPlayerUserId(m_pEdict), engine->IndexOfEdict(blocker), bestDist);
	return true;
}

// player_say event entry (bot_events.cpp). A bot speaker counts only for the test command.
int s_testWagesSpeaker = -1;
void EmpiresHandleWagesChatEvent(edict_t* speaker, const char* text)
{
	const bool test = s_testWagesSpeaker >= 0 && engine->GetPlayerUserId(speaker) == s_testWagesSpeaker;
	if (test)
		s_testWagesSpeaker = -1;
	HandleWagesChat(speaker, text, test);
}

// Test only (rcbotd empires_test_wages <speaker userid> <wages>): gives every other bot of the
// speaker's team that much wages, then runs the chat handler as if the speaker wrote "wages".
int EmpiresTestWagesChat(int speakerUserId, float wages)
{
	if (!EmpiresBinaryOffsetsTrusted())
		return -1;
	edict_t* speaker = nullptr;
	for (int i = 1; i <= gpGlobals->maxClients && speaker == nullptr; ++i)
	{
		edict_t* e = engine->PEntityOfEntIndex(i);
		if (e != nullptr && !e->IsFree() && engine->GetPlayerUserId(e) == speakerUserId)
			speaker = e;
	}
	if (speaker == nullptr)
		return -1;
	int set = 0;
	for (int slot = 0; slot < RCBOT_MAXPLAYERS; ++slot)
	{
		CBot* bot = CBots::getBot(slot);
		edict_t* e = bot != nullptr && bot->inUse() ? bot->getEdict() : nullptr;
		if (e == nullptr || e == speaker || CClassInterface::getTeam(e) != CClassInterface::getTeam(speaker))
			continue;
		*reinterpret_cast<float*>(reinterpret_cast<char*>(e->GetUnknown()->GetBaseEntity()) + 0x12c8) = wages;
		e->m_fStateFlags |= FL_EDICT_CHANGED | FL_FULL_EDICT_CHANGED; // StateChanged() needs engine-only symbols
		++set;
	}
	// Through the real chat: the bot says it, the game fires player_say.
	s_testWagesSpeaker = speakerUserId;
	if (helpers != nullptr)
		helpers->ClientCommand(speaker, "say test wages please\n");
	return set;
}

// Test only (rcbotd empires_vehicle_scalehp <factor>): scales the health of
// every non-commander vehicle. Returns how many were changed.
int EmpiresVehicleDebugScaleHealth(float factor, const float* sideFactors)
{
	if (gpGlobals == nullptr || engine == nullptr)
		return 0;
	int changed = 0;
	for (int i = gpGlobals->maxClients + 1; i < gpGlobals->maxEntities; ++i)
	{
		edict_t* e = engine->PEntityOfEntIndex(i);
		if (!IsClass(e, "emp_vehicle"))
			continue;
		int* hp = VehicleHealthField(e);
		if (hp == nullptr || *hp <= 0)
			continue;
		const int before = *hp;
		*hp = (std::max)(1, static_cast<int>(before * factor));
		float total = -1.0f;
		ReadEntityFloat(e, "m_flTotalHealth", total);
		int armor[4] = {-1, -1, -1, -1};
		if (float* sides = VehicleArmorSides(e))
		{
			int* mirror = reinterpret_cast<int*>(reinterpret_cast<char*>(sides) + 0x20); // +0x1004
			for (int k = 0; k < 4; ++k)
			{
				if (sideFactors != nullptr && sides[k] > 0.0f)
				{
					sides[k] = (std::max)(1.0f, std::floor(sides[k] * sideFactors[k]));
					mirror[k] = static_cast<int>(sides[k]);
				}
				armor[k] = static_cast<int>(sides[k]);
			}
		}
		RCBOT_DIAG("[RCBOT2] vehicle debug-hp veh=%d health=%d->%d total=%.0f armor=%d/%d/%d/%d\n", i, before, *hp,
			total, armor[0], armor[1], armor[2], armor[3]);
		++changed;
	}
	return changed;
}

// Test only (rcbotd empires_vehicle_setammo <magazine> <reserve>): sets every weapon of every
// vehicle with a turret. The tank's own counters (+0x90c magazine, +0x92c reserve, 8 slots,
// CEmpTank::GiveAmmo 0x697ce0) are what the game uses; the turret arrays are its network copies.
int EmpiresVehicleDebugSetAmmo(int magazine, int reserve)
{
	if (gpGlobals == nullptr || engine == nullptr || !EmpiresBinaryOffsetsTrusted())
		return 0;
	int changed = 0;
	for (int i = gpGlobals->maxClients + 1; i < gpGlobals->maxEntities; ++i)
	{
		edict_t* e = engine->PEntityOfEntIndex(i);
		if (!IsClass(e, "emp_vehicle") || e->GetUnknown() == nullptr || e->GetUnknown()->GetBaseEntity() == nullptr)
			continue;
		const char* className = e->GetClassName();
		if (className != nullptr && std::strstr(className, "commander") != nullptr)
			continue;
		int turretIndex = -1;
		CBaseHandle* handle = nullptr;
		{
			IServerNetworkable* net = e->GetUnknown()->GetNetworkable();
			ServerClass* cls = net != nullptr ? net->GetServerClass() : nullptr;
			unsigned offset = 0;
			if (cls != nullptr && UTIL_FindSendPropInfo(cls, "m_hTurret", &offset) && offset > 0)
				handle = reinterpret_cast<CBaseHandle*>(reinterpret_cast<char*>(e->GetUnknown()->GetBaseEntity()) + offset);
		}
		if (handle != nullptr && handle->IsValid())
			turretIndex = handle->GetEntryIndex();
		edict_t* turret = turretIndex > 0 && turretIndex < gpGlobals->maxEntities ? engine->PEntityOfEntIndex(turretIndex) : nullptr;
		std::int32_t weapons[8], ammo[8], clips[8];
		if (turret == nullptr || !ReadTurretArray(turret, "m_iWeapon", weapons))
			continue;
		char* tank = reinterpret_cast<char*>(e->GetUnknown()->GetBaseEntity());
		for (int k = 0; k < 8; ++k)
		{
			const bool used = weapons[k] >= 0 && weapons[k] <= 42;
			ammo[k] = used ? magazine : 0;
			clips[k] = used ? reserve : 0;
			if (used)
			{
				reinterpret_cast<int*>(tank + 0x90c)[k] = magazine;
				reinterpret_cast<int*>(tank + 0x92c)[k] = reserve;
			}
		}
		if (!ReadTurretArray(turret, "m_iWeaponAmmo", ammo, true) || !ReadTurretArray(turret, "m_iWeaponClips", clips, true))
			continue;
		turret->m_fStateFlags |= FL_EDICT_CHANGED | FL_FULL_EDICT_CHANGED; // StateChanged() needs engine-only symbols
		RCBOT_DIAG("[RCBOT2] vehicle debug-ammo veh=%d turret=%d magazine=%d reserve=%d\n", i, turretIndex, magazine, reserve);
		++changed;
	}
	return changed;
}

// Test only (rcbotd empires_vehicle_crate <distance>): puts an emp_eng_crate of the vehicle's team
// <distance> units beside the first non-commander vehicle, on the navmesh (3.10b step 2).
extern IServerTools* servertools;
static int* DebugTeamField(edict_t* e)
{
	IServerNetworkable* net = e != nullptr && e->GetUnknown() != nullptr ? e->GetUnknown()->GetNetworkable() : nullptr;
	ServerClass* cls = net != nullptr ? net->GetServerClass() : nullptr;
	unsigned offset = 0;
	if (cls == nullptr || e->GetUnknown()->GetBaseEntity() == nullptr || !UTIL_FindSendPropInfo(cls, "m_iTeamNum", &offset) ||
		offset == 0)
		return nullptr;
	return reinterpret_cast<int*>(reinterpret_cast<char*>(e->GetUnknown()->GetBaseEntity()) + offset);
}

static int DebugReadTeam(edict_t* e)
{
	const int* field = DebugTeamField(e);
	return field != nullptr ? *field : -1;
}

int EmpiresVehicleDebugCrate(float distance)
{
	if (gpGlobals == nullptr || engine == nullptr || servertools == nullptr)
		return -1;
	for (int i = gpGlobals->maxClients + 1; i < gpGlobals->maxEntities; ++i)
	{
		edict_t* e = engine->PEntityOfEntIndex(i);
		if (!IsClass(e, "emp_vehicle"))
			continue;
		const char* className = e->GetClassName();
		if (className != nullptr && std::strstr(className, "commander") != nullptr)
			continue;
		const int team = DebugReadTeam(e);
		if (team != 2 && team != 3)
			continue;
		const Vector at = CBotGlobals::entityOrigin(e);
		Vector spot = at;
		for (int k = 0; k < 8; ++k)
		{
			const float yaw = 45.0f * k;
			Vector tryAt = at + YawDir(yaw) * distance;
			Vector onMesh;
			if (RCBotNavMesh::ClosestPointOnMesh(tryAt, 300.0f, onMesh) && std::fabs(Dist2D(onMesh, at) - distance) < 300.0f)
			{
				spot = onMesh;
				break;
			}
		}
		CBaseEntity* crate = servertools->CreateEntityByName("emp_eng_crate");
		if (crate == nullptr)
			return -1;
		char buf[64];
		snprintf(buf, sizeof(buf), "%d", team);
		servertools->SetKeyValue(crate, "TeamNum", buf);
		servertools->SetKeyValue(crate, "origin", spot + Vector(0, 0, 16));
		servertools->DispatchSpawn(crate);
		edict_t* ce = nullptr;
		for (int k = gpGlobals->maxClients + 1; k < gpGlobals->maxEntities && ce == nullptr; ++k)
		{
			edict_t* c = engine->PEntityOfEntIndex(k);
			if (IsClass(c, "emp_eng_crate") && c->GetUnknown() != nullptr && c->GetUnknown()->GetBaseEntity() == crate)
				ce = c;
		}
		const int ci = ce != nullptr ? engine->IndexOfEdict(ce) : -1;
		// The TeamNum keyvalue does not reach it (run 234730: read 0); set the field directly.
		if (int* field = DebugTeamField(ce))
		{
			*field = team;
			ce->m_fStateFlags |= FL_EDICT_CHANGED | FL_FULL_EDICT_CHANGED;
		}
		RCBOT_DIAG("[RCBOT2] vehicle debug-crate veh=%d crate=%d team=%d readteam=%d at=(%.0f %.0f %.0f) dist=%.0f\n", i, ci, team,
			ce != nullptr ? DebugReadTeam(ce) : -1, spot.x, spot.y, spot.z, Dist2D(spot, at));
		return ci;
	}
	return -1;
}

void EmpiresVehicleLevelReset()
{
	for (RepairRequest& r : s_repairRequest)
		r = RepairRequest();
	for (WreckClaim& c : s_wreckClaim)
		c = WreckClaim();
	for (auto& team : s_ammoRequest)
		for (EmpiresAmmoRequest& r : team)
			r = EmpiresAmmoRequest();
	g_pushZones.clear();
	g_pushZonesAt = -1.0f;
	g_obstacleTest = VehicleObstacleTest();
	g_maneuverPerf = ManeuverPerf();
	g_maneuverTick = -1;
	g_maneuverTickCalls = 0;
}

// Test-only: arm an obstacle for the next NF Light/BE AFV driver (see VehicleObstacleTest).
bool EmpiresVehicleDebugObstacle(float ahead, float lateral, float entrySpeed, const char* model)
{
	if (servertools == nullptr || enginetrace == nullptr)
		return false;
	g_obstacleTest = VehicleObstacleTest();
	g_obstacleTest.armed = true;
	g_obstacleTest.ahead = (std::max)(300.0f, ahead);
	g_obstacleTest.lateral = lateral;
	g_obstacleTest.entrySpeed = entrySpeed;
	if (model != nullptr && *model != '\0')
		snprintf(g_obstacleTest.model, sizeof(g_obstacleTest.model), "%s", model);
	RCBOT_DIAG("[RCBOT2] vehicle obstacle armed ahead=%.0f lateral=%.0f entry=%.0f model=%s\n",
		g_obstacleTest.ahead, lateral, entrySpeed, g_obstacleTest.model);
	return true;
}
