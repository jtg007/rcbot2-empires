// Empires vehicles: buy, enter, drive, fight, run over, Bill Nye.
// Game facts (inputs, purchase commands, chassis/engine indices, stun grenade):
// reverse-engineering/notes/vehicle-driving.md.

#include "bot_empires.h"
#include "bot_empires_binary.h"
#include "bot_empires_team_economy.h"
#include "bot_buttons.h"
#include "bot_cvars.h"
#include "bot_globals.h"
#include "bot_getprop.h"
#include "server_class.h"
#include "bot_navmesh_api.h"
#include "bot_mtrand.h"
#include "bot_schedule.h"
#include "in_buttons.h"
#include "engine/IEngineTrace.h"
#include "iplayerinfo.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

bool EmpiresTeamResources(int team, float& resources);  // bot_empires_commander_economy.cpp

extern IPlayerInfoManager* playerinfomanager;

namespace
{
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
		std::snprintf(expected, sizeof(expected), "%03u", static_cast<unsigned>(i));
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
	std::snprintf(target, sizeof(target), "#%d", speakerId);
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
			std::snprintf(command, sizeof(command), "say !give %s\n", target);
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
bool GroundAt(const Vector& p, float refZ)
{
	if (enginetrace == nullptr)
		return true;
	Ray_t ray;
	ray.Init(Vector(p.x, p.y, refZ + kRoadProbeUp), Vector(p.x, p.y, refZ - kRoadProbeDown));
	CTraceFilterWorldOnly filter;
	trace_t hit;
	enginetrace->TraceRay(ray, MASK_PLAYERSOLID_BRUSHONLY, &filter, &hit);
	return hit.fraction < 1.0f && !hit.allsolid;
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
	const float pushLook = std::max(lookAhead, 350.0f);
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
bool DriveLineClear(const Vector& from, const Vector& to, float halfWidth)
{
	Vector dir = to - from;
	dir.z = 0.0f;
	const float len = dir.Length();
	if (len < 1.0f)
		return true;
	dir /= len;
	const Vector side(-dir.y, dir.x, 0.0f);
	CTraceFilterWorldOnly filter;
	for (const float s : {-1.0f, 1.0f})
	{
		const Vector a = from + side * (s * halfWidth) + Vector(0, 0, 48);
		const Vector b = to + side * (s * halfWidth) + Vector(0, 0, 48);
		trace_t hit;
		Ray_t ray;
		ray.Init(a, b);
		enginetrace->TraceRay(ray, MASK_PLAYERSOLID, &filter, &hit);
		if (hit.fraction < 1.0f)
			return false;
	}
	return true;
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
};
RepairRequest s_repairRequest[4];

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
		local[i] = std::max(mins[i], std::min(maxs[i], local[i]));
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
	v.weaponSummary[0] = '\0';
	std::int32_t weapons[8], ammo[8], clips[8];
	std::uint8_t modes[8], groups[48];
	if (turret == nullptr || !ReadTurretArray(turret, "m_iWeapon", weapons) ||
		!ReadTurretArray(turret, "m_iWeaponAmmo", ammo) || !ReadTurretArray(turret, "m_iWeaponClips", clips) ||
		!ReadTurretArray(turret, "m_bWeaponMode", modes) || !ReadTurretArray(turret, "m_bWeaponGroup", groups))
		return;
	const int group = EmpiresVehicleReadInt(bot, turret, "m_iGroup", -1);
	if (group < 0 || group > 5)
		return;
	int other = 0; // buttons that also fire something other than a machine gun
	int mgLoaded = 0;
	std::size_t used = 0;
	for (int i = 0; i < 8; ++i)
	{
		const int id = weapons[i];
		if (id < 0 || id > 42 || !groups[i * 6 + group])
			continue;
		const int button = modes[i] ? IN_ATTACK : IN_ATTACK2;
		const bool loaded = ammo[i] > 0;
		const bool mg = id <= 11;
		if (mg)
		{
			if (loaded)
				mgLoaded |= button;
			if (ammo[i] > 0 || clips[i] > 0)
				v.weaponMgLeft = true;
		}
		else if (ammo[i] > 0 || clips[i] > 0)
			other |= button; // an empty missile pod must not block the MG on its button
		if (loaded && ((id >= 12 && id <= 22) || id >= 40))
			v.weaponHeavyInf |= button;
		if ((id >= 12 && id <= 22) || (id >= 27 && id <= 39))
		{
			v.weaponHeavyPresent = true;
			if (ammo[i] > 0 || clips[i] > 0)
				v.weaponHeavyLeft = true;
		}
		if (loaded && ((id >= 12 && id <= 22) || (id >= 27 && id <= 39) || id == 1 || id == 4 || id == 5 || (id >= 8 && id <= 11)))
			v.weaponArmour |= button;
		if (used < sizeof(v.weaponSummary))
		{
			const int n = std::snprintf(v.weaponSummary + used, sizeof(v.weaponSummary) - used, "%s%d:%s:%d/%d",
				used ? "," : "", id, button == IN_ATTACK ? "a1" : "a2", ammo[i], clips[i]);
			if (n > 0)
				used += static_cast<std::size_t>(n);
		}
	}
	v.weaponMgPure = mgLoaded & ~other;
	v.weaponMgMixed = mgLoaded & other;
	v.weaponsKnown = true;
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

} // namespace

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
		return edict == m_target || index <= 0 || index > gpGlobals->maxClients;
	}

private:
	edict_t* m_self;
	edict_t* m_vehicle;
	edict_t* m_target;
};

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
	for (Vector& point : route)
		point.z = from.z;
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
			const int n = std::snprintf(why + used, whySize - used, "side%d yaw=%.0f ext=%.0f reach=%.0f ss=%d hit=%s %s; ",
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
					start.z = std::min(start.z + (end.z - start.z) * hit.fractionleftsolid, start.z) - 2.0f;
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
		std::snprintf(why, whySize, "surfaces=%d blocked=%d not_factory=%d bounds=(%.0f %.0f %.0f)-(%.0f %.0f %.0f)",
			surfaces, blocked, notFactory, mins.x, mins.y, mins.z, maxs.x, maxs.y, maxs.z);
	return candidates > 0;
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
			return (mins + maxs) * 0.5f;
	}
	return origin + Vector(0, 0, 40);
}
} // namespace

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
	if (commanderVehicleEntryActive() || m_iBuildAssignStage > 0 ||
		isEmpiresCommanderMoveActive() || isEmpiresVehicleActionActive())
	{
		if (v.stage != EmpiresVehicleStage::NONE &&
			v.stage != EmpiresVehicleStage::DRIVING)
			resetEmpiresVehicle("other-owner", 5.0f);
		if (v.stage == EmpiresVehicleStage::NONE)
			return;
	}

	const int userId = engine->GetPlayerUserId(m_pEdict);
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

	// ---------------------------------------------------------------- decide
	if (v.stage == EmpiresVehicleStage::NONE)
	{
		if (now < v.nextDecision || now < v.cooldownUntil || !rcbot_empires_vehicles.GetBool())
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
			if (seat != nullptr && IsClass(seat, "emp_vehicle") &&
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
			skip = "in-vehicle";
		}
		else if (isEmpiresEngineerClass())
		{
			skip = "engineer";
			edict_t* hurt = RepairRequestVehicle(team);
			RepairRequest& r = s_repairRequest[team];
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
		if (skip != nullptr)
		{
			if (now >= v.nextLog)
			{
				v.nextLog = now + 20.0f;
				RCBOT_DIAG("[RCBOT2] vehicle skip userid=%d reason=%s vehicle=%d\n", userId, skip, inVehicle);
			}
			return;
		}

		const bool billNye = isEmpiresBillNye();
		if (!billNye)
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
				std::max(2, (teamBots * 2 + 2) / 5);
			if (drivers >= limit)
				return;
		}

		// An empty own vehicle close by is used before buying a new one, but only one parked at
		// the own base (CV or building within kBaseRadius) that stood without a driver for
		// kEmptyFor (TODO 3.10e, user: bots took vehicles left anywhere a moment ago).
		constexpr float kBaseRadius = 1500.0f;
		constexpr float kEmptyFor = 30.0f;
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
		float bestDistance = billNye ? 0.0f : 2500.0f;
		for (int i = gpGlobals->maxClients + 1; i < gpGlobals->maxEntities && !billNye; ++i)
		{
			edict_t* e = engine->PEntityOfEntIndex(i);
			if (!IsClass(e, "emp_vehicle") || EmpiresVehicleReadInt(this, e, "m_iTeamNum", -1) != team ||
				IsCommanderChassis(EmpiresVehicleReadInt(this, e, "m_iChassis", -1)) || !EmpiresVehicleAlive(this, e))
				continue;
			int driver = -1;
			if (!readEmpiresEntityHandleProperty(e, "m_hPassengers", 0, driver))
				continue;
			// Empty since: per entity index and serial. A driver or a move since the last look
			// (driven away and parked again while nobody checked) restarts it.
			static float s_emptySince[MAX_EDICTS];
			static Vector s_emptyAt[MAX_EDICTS];
			static int s_emptySerial[MAX_EDICTS];
			const int serial = e->m_NetworkSerialNumber;
			const Vector at = CBotGlobals::entityOrigin(e);
			if (driver >= 0 || s_emptySerial[i] != serial || (s_emptyAt[i] - at).Length() > 100.0f || s_emptySince[i] > now ||
				s_emptySince[i] <= 0.0f)
			{
				s_emptySerial[i] = serial;
				s_emptySince[i] = now;
			}
			s_emptyAt[i] = at;
			float baseDistance = 1.0e9f;
			for (const Vector& p : basePoints)
				baseDistance = std::min(baseDistance, (p - at).Length2D());
			if (driver >= 0 || now - s_emptySince[i] < kEmptyFor || baseDistance >= kBaseRadius)
			{
				if (now >= v.nextEmptyLog)
				{
					v.nextEmptyLog = now + 10.0f;
					RCBOT_DIAG("[RCBOT2] vehicle empty-skip userid=%d vehicle=%d driver=%d empty_for=%.0f base_dist=%.0f dist=%.0f\n", userId,
						i, driver, now - s_emptySince[i], baseDistance, distanceFrom(at));
				}
				continue;
			}
			// Another bot may already be walking to it.
			bool claimed = false;
			for (int slot = 0; slot < RCBOT_MAXPLAYERS && !claimed; ++slot)
			{
				CBot* bot = CBots::getBot(slot);
				if (bot == nullptr || !bot->inUse())
					continue;
				const EmpiresVehicleBrain& other = static_cast<const CBotEmpires*>(bot)->m_vehicle;
				claimed = (bot != this && other.vehicleIndex == i) ||
					(other.abandonedVehicle == i && now < other.abandonedUntil); // stuck, left behind
			}
			const float distance = distanceFrom(CBotGlobals::entityOrigin(e));
			if (!claimed && distance < bestDistance)
			{
				bestDistance = distance;
				best = e;
			}
		}

		if (now >= v.nextEmptyLog)
			v.nextEmptyLog = now + 10.0f;
		v.billNye = billNye;
		if (best != nullptr)
		{
			v.vehicle = best;
			v.vehicleIndex = engine->IndexOfEdict(best);
			setStage(EmpiresVehicleStage::APPROACH, 40.0f);
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
			if (!EmpiresTeamEconomy::PurchaseAllowed(wages, pool, money.vehicleCost, reserve))
			{
				if (now >= v.nextLog)
				{
					v.nextLog = now + 20.0f;
					RCBOT_DIAG("[RCBOT2] vehicle skip userid=%d reason=team-reserve wages=%.0f pool=%.0f cost=%.0f reserve=%.0f\n",
						userId, wages, pool, money.vehicleCost, reserve);
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
			if (EmpiresTeamEconomy::DeferToRicherBuyer(wages, richest, money.vehicleCost, now - v.deferredSince, 30.0f))
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
			setStage(EmpiresVehicleStage::GOTO_FACTORY, 90.0f);
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
	// Keep trying to leave the same vehicle even after the deadline. Resetting
	// while still seated leaves the bot unable to plan or exit (run 125334).
	if (v.stageUntil > 0.0f && now > v.stageUntil && v.stage != EmpiresVehicleStage::GIVE_UP_EXIT)
	{
		++v.failures;
		if (v.failures >= 2 && !v.billNye)
			v.wantJeep = true;
		resetEmpiresVehicle("stage-timeout", 10.0f * std::min(v.failures, 6));
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
			if (RCBotNavMesh::IsLoaded() && now >= v.nextAction)
			{
				v.nextAction = now + 4.0f;
				requestNavigation(origin, RCBotNavigation::NavigationPolicy::NAVMESH_ONLY, 0);
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
		const bool jeep = v.billNye || v.wantJeep;
		const bool nf = team == 2;
		char command[96] = {};
		switch (v.purchaseStep)
		{
		case 0:
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
			std::snprintf(command, sizeof(command), "emp_vehicle_chassis %d\n",
				jeep ? (nf ? 0 : 8) : (nf ? 2 : 10));
			break;
		case 1:
			// Cannon (weapon 12, mode 1 = IN_ATTACK). NF light tank slot 0 is the
			// cannon, BE AFV slot 1 (scripts/vehicle_*.txt weapon_slot order).
			if (!jeep)
				std::snprintf(command, sizeof(command), "emp_vehicle_weapon %d 0 12 1 0 0 0 0 0 1\n", nf ? 0 : 1);
			break;
		case 2:
			// Machine gun (weapon 0, mode 0 = IN_ATTACK2) in the other slot.
			if (!jeep)
				std::snprintf(command, sizeof(command), "emp_vehicle_weapon %d 0 0 1 0 0 0 0 0 %d\n", nf ? 1 : 0,
					rcbot_empires_vehicle_debug_mixed.GetBool() ? 1 : 0);
			break;
		case 3:
			std::snprintf(command, sizeof(command), "emp_vehicle_armor 0 0\n");
			break;
		case 4:
			// Plain armor on every side so one hit side is not the hull at once
			// (script max: NF light tank 2, BE AFV 3, jeeps 1).
			if (jeep)
				std::snprintf(command, sizeof(command), "emp_vehicle_armor_levels 0 0 0 0\n");
			else
				std::snprintf(command, sizeof(command), "emp_vehicle_armor_levels 2 2 2 2\n");
			break;
		case 5:
			std::snprintf(command, sizeof(command), "emp_vehicle_engine 0 %d\n",
				jeep ? (nf ? 1 : 0) : 13);
			break;
		case 6:
			if (!v.buyInPlace)
				std::snprintf(command, sizeof(command), "emp_vehicle_factory %d\n", v.factoryIndex);
			break;
		case 7:
			// Diagnostic: production purchases never spawned a vehicle (run 103144).
			LogEmpiresFactoryProbe("buy", userId, m_pEdict, v.factoryIndex);
			LogEmpiresVehicleEcon("before", userId, getTeam(), m_pEdict);
			v.factoryProbeAt = now + 1.0f;
			std::snprintf(command, sizeof(command), "emp_vehicle_build\n");
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
					std::snprintf(list + used, sizeof(list) - used, " %d:%s:t%d", i, e->GetClassName(),
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
		const float distance = Dist2D(getOrigin(), center);
		if (v.stage == EmpiresVehicleStage::APPROACH)
		{
			if (distance > kEnterDistance)
			{
				setMoveTo(center);
				return;
			}
			stopMoving();
			v.useTries = 0;
			v.nextAction = now;
			setStage(EmpiresVehicleStage::ENTERING, 8.0f);
		}
		// ENTERING: look at the body and tap use.
		VectorAngles(center - getEyePosition(), v.view);
		v.view.z = 0.0f;
		if (distance > kEnterDistance + 120.0f)
		{
			setStage(EmpiresVehicleStage::APPROACH, 30.0f);
			return;
		}
		if (distance > 110.0f)
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
			resetEmpiresVehicle("gave-up-stuck", 30.0f);
			v.bailLogAt = now + 0.3f; // after the reset, which clears the whole brain
			v.bailLogStep = 1;
			return;
		}
		if (inVehicle != v.vehicleIndex)
		{
			resetEmpiresVehicle("vehicle-changed", 5.0f);
			return;
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
		if (hurt == nullptr || hurt->IsFree() || RepairRequestVehicle(team) != hurt ||
			(v.stageUntil > 0.0f && now > v.stageUntil))
		{
			if (r.engineerUserId == userId)
				r.engineerUserId = -1;
			resetEmpiresVehicle(hurt == nullptr || RepairRequestVehicle(team) != hurt ? "repair-done" :
				"repair-timeout", 3.0f);
			return;
		}
		r.engineerUserId = userId;
		r.claimUntil = now + 10.0f;
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
			total += std::max(0, ReadArmorSide(hurt, k));
		if (v.assistSide < 0)
		{
			v.assistSide = r.side >= 0 ? r.side : 0;
			v.assistLastRise = now;
			v.assistLastTotal = total;
		}
		if (total > v.assistLastTotal + 0.5f)
		{
			v.assistLastRise = now;
			// A side takes ~27 s (126 armor at ~4.7/s); 60 s cut the job after
			// two sides (run 120808). Keep going while it mends.
			v.stageUntil = std::max(v.stageUntil, now + 20.0f);
		}
		v.assistLastTotal = total;
		matrix3x4_t toWorld;
		AngleMatrix(box->GetCollisionAngles(), box->GetCollisionOrigin(), toWorld);
		const Vector mins = box->OBBMins(), maxs = box->OBBMaxs();
		const Vector mid = (mins + maxs) * 0.5f;
		const float hx = (maxs.x - mins.x) * 0.5f, hy = (maxs.y - mins.y) * 0.5f;
		Vector me;
		VectorITransform(getOrigin(), toWorld, me);
		const float rx = (me.x - mid.x) / std::max(1.0f, hx), ry = (me.y - mid.y) / std::max(1.0f, hy);
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
				v.assistLastRise = std::max(v.assistLastRise, now - 2.0f); // walking is not "no progress"
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
		v.view.x -= std::min(25.0f, distanceFrom(aim) / 40.0f); // small lob
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
	const float dt = now - v.lastSampleTime;
	if (dt >= 0.1f)
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
	const bool armed = chassis != 0 && chassis != 8 && !v.commanderCV;

	// -------- target choice
	if (now >= v.nextTargetPick)
	{
		v.nextTargetPick = now + 0.5f;
		edict_t* best = nullptr;
		float bestScore = 1.0e9f;
		// Only what a driver could see (rainbowroad 11 vs 11: tanks shot for minutes at an
		// empty jeep and through cover, user: "shoot at enemies that are not there").
		// The current target stays while seen within the last 3 s.
		const Vector eye = origin + Vector(0, 0, 90);
		auto inSight = [&](edict_t* e) {
			if (e == v.target.get() && now - v.fightSight < 3.0f)
				return true;
			trace_t hit;
			Ray_t ray;
			ray.Init(eye, EntityCenter(e));
			CTraceFilterWorldOnly filter;
			enginetrace->TraceRay(ray, MASK_SHOT, &filter, &hit);
			return hit.fraction > 0.97f;
		};
		edict_t* bestEmpty = nullptr;
		float bestEmptyScore = 1500.0f; // empty enemy vehicles only close by, after everything else
		// Enemy vehicles: Bill Nye pushes them, armed vehicles shoot them.
		if (v.billNye || armed)
		{
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
				const float score = Dist2D(origin, at);
				if (!v.billNye && !enemyCommander)
				{
					int driver = -1;
					const bool empty = !readEmpiresEntityHandleProperty(e, "m_hPassengers", 0, driver) || driver < 0;
					// Enemy armour first, but only within cannon range (user: tanks first when in
					// range, then infantry).
					if (score >= 3500.0f || !inSight(e))
						continue;
					if (empty)
					{
						if (score < bestEmptyScore)
						{
							bestEmptyScore = score;
							bestEmpty = e;
						}
						continue;
					}
				}
				if (score < (v.billNye ? 20000.0f : 4000.0f) && score < bestScore)
				{
					bestScore = score;
					best = e;
				}
			}
		}
		// Enemy infantry on the ground: run them over or shoot them.
		if (best == nullptr)
		{
			for (int i = 1; i <= gpGlobals->maxClients; ++i)
			{
				edict_t* e = engine->PEntityOfEntIndex(i);
				IPlayerInfo* info = e != nullptr && !e->IsFree() && playerinfomanager != nullptr ?
					playerinfomanager->GetPlayerInfo(e) : nullptr;
				if (info == nullptr || info->IsDead() || info->GetTeamIndex() != enemyTeam ||
					EmpiresVehiclePlayerVehicle(this, e) >= 0)
					continue;
				const Vector at = CBotGlobals::entityOrigin(e);
				if (std::fabs(at.z - origin.z) > 160.0f)
					continue;
				const float score = Dist2D(origin, at);
				if (score < 3000.0f && score < bestScore && inSight(e)) // MG range (FalloffEnd 3000)
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
			const char* building = enemyTeam == 2 ? "emp_building_nf_" : "emp_building_imp_";
			for (int i = gpGlobals->maxClients + 1; i < gpGlobals->maxEntities; ++i)
			{
				edict_t* e = engine->PEntityOfEntIndex(i);
				if (e == nullptr || e->IsFree() || e->GetClassName() == nullptr)
					continue;
				const char* name = e->GetClassName();
				const bool isCv = std::strcmp(name, cv) == 0;
				const bool isBuilding = std::strncmp(name, building, std::strlen(building)) == 0 &&
					std::strstr(name, "physbox") == nullptr && std::strstr(name, "panel") == nullptr;
				if (!isCv && !isBuilding)
					continue;
				if (isBuilding && EmpiresVehicleReadInt(this, e, "m_iHealth", 1) <= 0)
					continue;
				const float score = Dist2D(origin, CBotGlobals::entityOrigin(e)) + (isCv ? 0.0f : 500.0f);
				if (score < 4000.0f && score < bestScore)
				{
					bestScore = score;
					best = e;
				}
			}
		}
		if (best != v.target.get())
		{
			v.aimSince = now;
			v.contactSince = 0.0f;
			v.ramming = false;
		}
		v.target = best;

		// Fallback goal: the enemy command vehicle or its start point.
		v.goalValid = false;
		for (int i = gpGlobals->maxClients + 1; i < gpGlobals->maxEntities && !v.goalValid; ++i)
		{
			edict_t* e = engine->PEntityOfEntIndex(i);
			if (IsClass(e, "emp_vehicle") && EmpiresVehicleReadInt(this, e, "m_iTeamNum", -1) == enemyTeam &&
				IsCommanderChassis(EmpiresVehicleReadInt(this, e, "m_iChassis", -1)))
			{
				v.goal = CBotGlobals::entityOrigin(e);
				v.goalValid = true;
			}
		}
		for (int i = gpGlobals->maxClients + 1; i < gpGlobals->maxEntities && !v.goalValid; ++i)
		{
			edict_t* e = engine->PEntityOfEntIndex(i);
			if (IsClass(e, enemyTeam == 2 ? "emp_nf_commander" : "emp_imp_commander"))
			{
				v.goal = CBotGlobals::entityOrigin(e);
				v.goalValid = true;
			}
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
			targetGone = info == nullptr || info->IsDead() || EmpiresVehiclePlayerVehicle(this, target) >= 0;
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
	float arrive = 600.0f;
	if (v.billNye && targetIsVehicle)
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
	else if (target != nullptr && armed)
	{
		// Rainbowroad 11 vs 11 (run 20261112-163322): tanks stopped at 1400 and stood still
		// 74-90 % of the time while shooting (user: fight badly, drive too little).
		// Now: close in until in sight and in range, then keep moving around the target
		// at an angle, front half toward it, and change direction every 4-8 s.
		const float distance = Dist2D(origin, targetAt);
		{
			trace_t hit;
			Ray_t ray;
			ray.Init(origin + Vector(0, 0, 90), EntityCenter(target));
			CTraceFilterWorldOnly filter;
			enginetrace->TraceRay(ray, MASK_SHOT, &filter, &hit);
			if (hit.fraction > 0.97f)
				v.fightSight = now;
		}
		const bool sight = now - v.fightSight < 3.0f;
		if (distance > 2200.0f || !sight)
		{
			driveTo = targetAt;
			arrive = sight ? 1600.0f : 300.0f;
		}
		else
		{
			if (v.fightDir == 0.0f || now >= v.fightSwitch)
			{
				v.fightDir = v.fightDir == 0.0f ? (randomFloat(0.0f, 1.0f) < 0.5f ? 1.0f : -1.0f) : -v.fightDir;
				v.fightSwitch = now + randomFloat(4.0f, 8.0f);
			}
			// Far: angle in. Close: drive across. Too close: open the range.
			const float offset = distance > 1700.0f ? 35.0f : distance < 900.0f ? 110.0f : 70.0f;
			const float bearing = YawTo(origin, targetAt);
			float yaw = NormalizeYaw(bearing + v.fightDir * offset);
			// Narrow road (rainbowroad +-1650): circle the way that has ground ahead.
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
			driveTo = origin + YawDir(yaw) * 700.0f;
			arrive = 0.0f;
			throttle = 0.7f;
			v.fightMove = true;
			if (now >= v.nextFightLog)
			{
				v.nextFightLog = now + 6.0f;
				RCBOT_DIAG("[RCBOT2] vehicle fight-move userid=%d target=%d dist=%.0f dir=%.0f offset=%.0f\n", userId,
					engine->IndexOfEdict(target), distance, v.fightDir, offset);
			}
		}
	}

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
	float total = std::max(0.0f, health);
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
		// 3.10d: an MG sharing its button with the cannon (adopted vehicle, other loadout) cannot be
		// used on infantry (mg-mixed); refit the own loadout on a pad. Only light tank / AFV: the
		// refit commands carry their slot order, armor and engine.
		if (!v.retreating && !v.billNye && !v.commanderCV && (chassis == 2 || chassis == 10) &&
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
				RCBOT_DIAG("[RCBOT2] vehicle loadout-check userid=%d veh=%d known=%d mixed=%d mg=%d heavy-left=%d weapons=%s\n",
					userId, v.vehicleIndex, v.weaponsKnown ? 1 : 0, v.weaponMgMixed, v.weaponMgPure,
					v.weaponHeavyLeft ? 1 : 0, v.weaponSummary);
			}
			else if (v.weaponsKnown && (v.weaponMgMixed != 0 || (v.weaponHeavyPresent && !v.weaponHeavyLeft)))
			{
				// 3.10b step 1: cannon/missiles out of magazine and reserve: the refit on the pad
				// refills every weapon (AddWeapon 0x69a090 sets magazine and reserve to max, RE).
				v.ammoTrip = v.weaponMgMixed == 0;
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
		if (!v.retreating && !v.billNye && !v.commanderCV && (hullFrac < 0.4f || armorGone) &&
			now >= v.retreatCooldownUntil)
		{
			v.retreating = true;
			v.lastHealth = total;
			v.lastHealthRise = now;
			v.nextReplan = 0.0f;
			v.parkCount = 0;
			v.padEntryAttempt = 0;
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
				v.lastHealthRise = std::max(v.lastHealthRise, now - 20.0f);
			// The hull may stop short of the highest value seen (run 220710:
			// 140/150, healed while waiting): a long plateau high up counts too.
			const bool plateau = now - v.lastHealthRise > 30.0f && hullFrac >= 0.8f && worstArmor >= 0.8f;
			// A loadout trip ends only after the refit was sent (healthy tanks would end at once).
			const bool refitPending = v.loadoutRefit && !v.refitDone;
			const char* end = refitPending ? (now - v.lastHealthRise > 180.0f ? "retreat-timeout" : nullptr) : (hullFrac >= 0.95f && worstArmor >= 0.9f) || plateau ? "repaired" :
				now - v.lastHealthRise > 120.0f ? "retreat-timeout" : nullptr;
			// Never park for good (user): after 20 s without repairs try again
			// (the stuck edge is blocked for vehicles, the other pad entrance
			// next); after three parks give the retreat up and fight on.
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
					RCBOT_DIAG("[RCBOT2] vehicle park-retry userid=%d veh=%d parks=%d pos=(%.0f %.0f)\n", userId,
						v.vehicleIndex, v.parkCount, origin.x, origin.y);
				}
			}
			if (end != nullptr)
			{
				RCBOT_DIAG("[RCBOT2] vehicle %s userid=%d veh=%d hp=%.0f/%.0f armor=%d/%d/%d/%d max=%d\n", end, userId,
					v.vehicleIndex, health, v.maxHealth, armor[0], armor[1], armor[2], armor[3], armorMax);
				v.retreating = false;
				v.parkValid = false;
				v.homeParkValid = false;
				if (v.loadoutRefit)
				{
					// Read the loadout again soon; a refused refit (money) is not retried for 2 min.
					v.loadoutCheckAt = now + 2.0f;
					v.loadoutRetryAt = now + 120.0f;
					v.loadoutVerify = true;
				}
				const bool wasLoadout = v.loadoutRefit;
				v.loadoutRefit = false;
				v.ammoTrip = false;
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
			}
		}
	}
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
		for (int i = gpGlobals->maxClients + 1; i < gpGlobals->maxEntities; ++i)
		{
			edict_t* e = engine->PEntityOfEntIndex(i);
			if (e == nullptr || e->IsFree() || e->GetClassName() == nullptr)
				continue;
			if (std::strstr(e->GetClassName(), "repairstation") != nullptr &&
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
		if (v.parkValid)
		{
			// Stuck on the way home (base gates/ramps only open from inside,
			// run 214613): wait where backing off ended, not at the obstacle
			// (engineers could not reach every side there, run 223745).
			driveTo = origin;
			arrive = 400.0f;
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
						if (RCBotNavMesh::BuildVehiclePath(origin, entry, route, v.width > 0.0f ? v.width : 150.0f) ==
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
							driveTo = pad + axis * std::max(0.0f, along - 350.0f);
							driveTo.z = pad.z;
						}
					}
				}
			}
			if (!v.refitDone && Dist2D(origin, pad) < 140.0f && std::fabs(forwardSpeed) < 60.0f &&
				now >= v.refitNext && helpers != nullptr)
			{
				const bool nf = team == 2;
				const bool jeep = chassis == 0 || chassis == 8;
				char command[96] = "";
				// Same loadout as bought: the game charges only the difference to the vehicle's
				// total cost (0x6550a7), so this refit is free and needs no reserve check
				// (run 214852: pool unchanged, armor 14 -> 140; RE vehicle-driving.md).
				switch (v.refitStep)
				{
				case 0: std::snprintf(command, sizeof(command), "emp_vehicle_chassis %d\n", chassis); break;
				case 1:
					if (!jeep)
						std::snprintf(command, sizeof(command), "emp_vehicle_weapon %d 0 12 1 0 0 0 0 0 1\n", nf ? 0 : 1);
					break;
				case 2:
					if (!jeep)
						std::snprintf(command, sizeof(command), "emp_vehicle_weapon %d 0 0 1 0 0 0 0 0 0\n", nf ? 1 : 0);
					break;
				case 3: std::snprintf(command, sizeof(command), "emp_vehicle_armor 0 0\n"); break;
				case 4:
					std::snprintf(command, sizeof(command), jeep ? "emp_vehicle_armor_levels 0 0 0 0\n" :
						"emp_vehicle_armor_levels 2 2 2 2\n");
					break;
				case 5:
					std::snprintf(command, sizeof(command), "emp_vehicle_engine 0 %d\n", jeep ? (nf ? 1 : 0) : 13);
					break;
				case 6: std::snprintf(command, sizeof(command), "emp_vehicle_build\n"); break;
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
		else if (v.loadoutRefit && (!haveHome || now - v.lastHealthRise > 30.0f))
		{
			// No finished own pad for 30 s (a new one may still be building: head home meanwhile,
			// run 20261114-2216): the loadout cannot be changed, fight on and look again later.
			v.retreating = false;
			v.loadoutRefit = false;
			v.ammoTrip = false;
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
				Vector spot = home + away * 700.0f;
				Vector onMesh;
				if (RCBotNavMesh::ClosestPointOnMesh(spot, 300.0f, onMesh))
					spot = onMesh;
				v.homePark = spot;
				v.homeParkFor = home;
				v.homeParkValid = true;
			}
			driveTo = v.homePark;
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
		if (arrived && s_repairRequest[team].vehicle == v.vehicleIndex)
		{
			s_repairRequest[team].until = std::max(s_repairRequest[team].until, now + 5.0f);
			s_repairRequest[team].armorDamaged = worstSide >= 0 && worstArmor < 0.95f;
			s_repairRequest[team].side = worstSide;
		}
	}

	if (v.commanderCV && target == nullptr)
		arrive = v.cvArrive;
	// -------- follow a navmesh route unless the goal is close and in sight
	if (v.width <= 0.0f)
	{
		v.width = (chassis == 0 || chassis == 8) ? 110.0f : 150.0f;
		if (vehicle->GetCollideable() != nullptr)
		{
			const Vector size = vehicle->GetCollideable()->OBBMaxs() - vehicle->GetCollideable()->OBBMins();
			if (size.x > 40.0f && size.x < 400.0f)
				v.width = size.x;
		}
		RCBOT_DIAG("[RCBOT2] vehicle width userid=%d veh=%d chassis=%d width=%.0f\n", userId, v.vehicleIndex,
			chassis, v.width);
	}
	const bool closeManeuver = (v.billNye && targetIsVehicle) ||
		(target != nullptr && Dist2D(origin, driveTo) < 900.0f &&
			DriveLineClear(origin, driveTo, v.width * 0.5f));
	bool onRoute = false;
	if (!closeManeuver && Dist2D(origin, driveTo) > 300.0f)
	{
		if (now >= v.nextReplan || Dist2D(v.routeGoal, driveTo) > 500.0f)
		{
			v.nextReplan = now + 4.0f;
			v.routeGoal = driveTo;
			RCBotNavMesh::Path path;
			const RCBotNavMesh::PathResult result =
				RCBotNavMesh::BuildVehiclePath(origin, driveTo, path, v.width, true);
			const bool ok = result == RCBotNavMesh::PathResult::SUCCESS && path.points.size() >= 2;
			if (static_cast<int>(result) != v.routeResult || (ok && v.route.empty()) || path.partial != v.routePartial)
				RCBOT_DIAG("[RCBOT2] vehicle route userid=%d result=%s%s points=%zu length=%.0f from=(%.0f %.0f %.0f) "
					"to=(%.0f %.0f %.0f) width=%.0f end_gap=%.0f\n", userId, RCBotNavMesh::PathResultName(result),
					path.partial ? "-partial" : "", path.points.size(), path.length, origin.x, origin.y, origin.z,
					driveTo.x, driveTo.y, driveTo.z, v.width,
					path.points.empty() ? -1.0f : Dist2D(path.points.back(), driveTo));
			v.routePartial = ok && path.partial;
			v.routeResult = static_cast<int>(result);
			v.routeFailed = !ok;
			v.route = ok ? path.points : std::vector<Vector>();
			v.routeAreas = ok ? path.areaIds : std::vector<unsigned int>();
			v.routeIndex = 1;
		}
		if (!v.route.empty())
		{
			// Advance past points we reached or passed.
			while (v.routeIndex + 1 < v.route.size())
			{
				const Vector& p = v.route[v.routeIndex];
				const Vector& q = v.route[v.routeIndex + 1];
				Vector seg = q - p;
				seg.z = 0.0f;
				Vector toP = p - origin;
				toP.z = 0.0f;
				if (Dist2D(origin, p) < std::max(160.0f, v.width) ||
					(DotProduct(toP, seg) < 0.0f && toP.Length() < 450.0f))
					++v.routeIndex;
				else
					break;
			}
			// Off the route: plan again soon.
			if (Dist2D(origin, v.route[v.routeIndex]) > 1200.0f)
				v.nextReplan = std::min(v.nextReplan, now + 0.5f);
			// Look ahead along the route as far as the body has a clear line.
			const float look = std::max(350.0f, std::min(1400.0f, 300.0f + speed * 0.7f));
			std::size_t pick = v.routeIndex;
			float along = Dist2D(origin, v.route[v.routeIndex]);
			for (std::size_t k = v.routeIndex + 1; k < v.route.size(); ++k)
			{
				along += Dist2D(v.route[k - 1], v.route[k]);
				if (along > look || std::fabs(v.route[k].z - origin.z) > 200.0f ||
					!DriveLineClear(origin, v.route[k], v.width * 0.5f))
					break;
				pick = k;
			}
			driveTo = v.route[pick];
			onRoute = true;
			if (pick + 1 < v.route.size() || Dist2D(origin, driveTo) > 300.0f || v.routePartial)
				arrive = 0.0f;
			// Slow down before a sharp corner.
			if (pick + 1 < v.route.size())
			{
				const float turn = std::fabs(NormalizeYaw(YawTo(v.route[pick], v.route[pick + 1]) -
					YawTo(origin, v.route[pick])));
				if (turn > 50.0f && Dist2D(origin, v.route[pick]) < 200.0f + speed * 0.8f)
					throttle = std::min(throttle, speed > 450.0f ? 0.0f : 0.6f);
			}
		}
	}
	else
	{
		v.route.clear();
		v.routeAreas.clear();
		v.nextReplan = 0.0f;
	}

	// -------- steering with edge avoidance
	const float goalDistance = Dist2D(origin, driveTo);
	const float desired = YawTo(origin, driveTo);
	// Bill Nye pushing a vehicle must get close to the edge; running over
	// infantry at speed needs to see the edge in time to stop.
	// Short only in contact: a missed ram at 900 u/s went over the map end
	// (run 202056, four Bill Nye falls).
	const bool shortLook = v.billNye && targetIsVehicle && v.ramming && Dist2D(origin, targetAt) < 450.0f;
	const float lookAhead = shortLook ? 200.0f :
		ram ? std::max(200.0f, std::min(1400.0f, speed * 0.9f)) :
		std::max(260.0f, std::min(1500.0f, speed * 1.1f + 200.0f));
	const float rearLook = std::max(220.0f, std::min(600.0f, speed * 0.6f));
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
			!SafeHeading(origin, velYaw + 45.0f, std::max(200.0f, drop)) &&
			!SafeHeading(origin, velYaw - 45.0f, std::max(200.0f, drop)))
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
		v.fightSwitch = std::min(v.fightSwitch, now);
	// Fighting with a weakened side: present the strongest side to the target.
	// Sides relative to the hull: 0 front, 1 rear, 2 left (+90), 3 right (-90).
	bool facing = false;
	if (armed && !v.retreating && target != nullptr && worstSide >= 0 && bestSide >= 0 &&
		worstArmor < 0.6f && bestSide != worstSide && Dist2D(origin, targetAt) < 2500.0f)
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
	float side = std::max(-1.0f, std::min(1.0f, -err / 30.0f)) * kMaxMove;
	if (std::fabs(err) > 70.0f)
		forward = kMaxMove * 0.4f;
	// Too fast for the turn: brake first (run 213421 swung through ±140° at 800 u/s).
	if (onRoute && std::fabs(err) > 40.0f && forwardSpeed > 380.0f)
		forward = -kMaxMove * 0.6f;
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
	const bool mayBack = !pivots && !v.billNye && !facing && !v.padInside &&
		!(arrive > 0.0f && goalDistance < arrive);
	if (mayBack || v.cvBackUntil > 0.0f)
	{
		const bool wantBack = std::fabs(err) > 110.0f || (!headingSafe && forwardSpeed < 60.0f);
		if (mayBack && v.cvBackUntil == 0.0f && now >= v.cvBackNext && wantBack && forwardSpeed < 200.0f &&
			rearSafe && found)
		{
			v.cvBackUntil = now + 3.0f;
			RCBOT_DIAG("[RCBOT2] vehicle back-turn start userid=%d err=%.0f speed=%.0f ahead_safe=%d\n", userId, err,
				forwardSpeed, headingSafe ? 1 : 0);
		}
		if (v.cvBackUntil > 0.0f)
		{
			const bool aligned = std::fabs(err) < 60.0f && headingSafe;
			if (aligned || now > v.cvBackUntil || !rearSafe)
			{
				RCBOT_DIAG("[RCBOT2] vehicle back-turn end userid=%d err=%.0f reason=%s\n", userId, err,
					aligned ? "aligned" : !rearSafe ? "rear" : "time");
				v.cvBackUntil = 0.0f;
				v.cvBackNext = now + 2.0f;
			}
			else
			{
				forward = -kMaxMove;
				side = err > 0.0f ? kMaxMove : -kMaxMove;
			}
		}
	}
	if (arrive > 0.0f && goalDistance < arrive)
		forward = 0.0f;
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
	if (v.commanderCV && target == nullptr && arrive > 0.0f)
	{
		if (goalDistance < arrive + 500.0f && forwardSpeed > 250.0f)
			forward = -kMaxMove * 0.6f;
		if (goalDistance < arrive)
		{
			side = 0.0f;
			forward = forwardSpeed > 60.0f ? -kMaxMove * 0.6f : forwardSpeed < -60.0f ? kMaxMove * 0.6f : 0.0f;
			if (std::fabs(forwardSpeed) < 60.0f && !v.cvArrived &&
				(!v.cvHunt || (v.cvNoPreySince > 0.0f && now - v.cvNoPreySince > 1.5f)))
			{
				v.cvArrived = true;
				v.cvEnd = "arrived";
				RCBOT_DIAG("[RCBOT2] cv-drive arrived userid=%d pos=(%.0f %.0f %.0f) dist=%.0f\n", userId, origin.x,
					origin.y, origin.z, goalDistance);
			}
		}
	}
	// On the repair pad: creep and stop at the centre (run 20261112-121820
	// crossed it at 350 u/s and swung into the station walls).
	if (v.retreating && v.padInside)
	{
		forward = std::min(forward, kMaxMove * (goalDistance < 500.0f ? 0.3f : 0.5f));
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

	if (!headingSafe && !v.billNye && !pivots && forward > 0.0f && v.cvBackUntil == 0.0f)
		forward = 0.0f; // never creep on toward an edge; the back-turn follows
	// Circling: the BE AFV reached 500-820 u/s and went over the edge (run 172359, 8 of 15
	// falls). Hold about 400 u/s in a fight.
	if (v.fightMove && forward > 0.0f)
	{
		if (forwardSpeed > 550.0f)
			forward = -kMaxMove * 0.5f;
		else if (forwardSpeed > 400.0f)
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

	// Stuck while throttling: back off with the wheels turned the other way.
	if (now < v.reverseUntil)
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
	else if (forward > 0.0f && speed < 40.0f && !facing && !(v.billNye && v.ramming))
	{
		if (v.slowSince == 0.0f)
			v.slowSince = now;
		else if (now - v.slowSince > 2.5f)
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
			if (v.stuckReverses > 4 && v.commanderCV)
			{
				// The commander keeps his CV: stop here, the economy decides (walk or give up).
				v.cvArrived = true;
				v.cvEnd = "stuck";
				RCBOT_DIAG("[RCBOT2] cv-drive stuck userid=%d pos=(%.0f %.0f %.0f)\n", userId, origin.x, origin.y, origin.z);
			}
			else if (v.stuckReverses > 4 && !(v.billNye && v.ramming))
			{
				RCBOT_DIAG("[RCBOT2] vehicle give-up userid=%d veh=%d pos=(%.0f %.0f %.0f) reverses=%d\n", userId,
					v.vehicleIndex, origin.x, origin.y, origin.z, v.stuckReverses - 1);
				v.abandonedVehicle = v.vehicleIndex;
				v.abandonedUntil = now + 120.0f;
				v.nextAction = now;
				setStage(EmpiresVehicleStage::GIVE_UP_EXIT, 10.0f);
				return;
			}
			if (onRoute)
			{
				const int edge = RCBotNavMesh::BlockVehicleEdgeAt(origin, v.routeAreas, 90.0f);
				RCBOT_DIAG("[RCBOT2] vehicle route-block userid=%d pos=(%.0f %.0f %.0f) edge=%d areas=%zu\n", userId,
					origin.x, origin.y, origin.z, edge, v.routeAreas.size());
				v.nextReplan = now + 1.5f; // after backing off
			}
			if (rearSafe)
			{
				v.reverseUntil = now + 1.5f;
				v.reverseSide = side >= 0.0f ? -kMaxMove : kMaxMove;
				if (v.fightMove)
					v.fightSwitch = now; // blocked while circling: other way round
				RCBOT_DIAG("[RCBOT2] vehicle stuck-reverse userid=%d pos=(%.0f %.0f %.0f) n=%d\n", userId,
					origin.x, origin.y, origin.z, v.stuckReverses);
			}
			else
				RCBOT_DIAG("[RCBOT2] vehicle stuck-no-rear userid=%d pos=(%.0f %.0f %.0f) n=%d\n", userId,
					origin.x, origin.y, origin.z, v.stuckReverses);
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
			v.stuckReverses = 0;
	}
	else
		v.movingSince = 0.0f;

	int buttons = 0;
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
	QAngle view(0, chosen, 0);
	if (target != nullptr)
	{
		const Vector eye = origin + Vector(0, 0, 90);
		const int targetIndex = engine->IndexOfEdict(target);
		const bool infantry = targetIndex >= 1 && targetIndex <= gpGlobals->maxClients;
		// Lead: target velocity from its last positions (smoothed).
		const Vector centre = EntityCenter(target);
		if (targetIndex != v.aimTarget)
		{
			v.aimTarget = targetIndex;
			v.aimVel = Vector(0, 0, 0);
			v.aimLastPos = centre;
			v.aimLastTime = now;
		}
		else if (now - v.aimLastTime >= 0.2f)
		{
			const Vector measured = (centre - v.aimLastPos) / (now - v.aimLastTime);
			v.aimVel = measured.Length() < 2500.0f ? v.aimVel * 0.5f + measured * 0.5f : Vector(0, 0, 0);
			v.aimLastPos = centre;
			v.aimLastTime = now;
		}
		// vehicle_weapons.txt: Standard Machine Gun 1000 u/s no gravity, no effect on armour,
		// falloff end 3000; Standard Cannon 3300 u/s, gravity 0.5, splash 250.
		// Infantry: MG, plus the cannon close in. Vehicles and buildings: cannon.
		const float range = (centre - eye).Length();
		const float flight = range / (infantry ? 1000.0f : 3300.0f);
		Vector aim = centre + v.aimVel * flight;
		if (!infantry)
			aim.z += 0.5f * 0.5f * 600.0f * flight * flight; // drop at sv_gravity 600 (assumed)
		VectorAngles(aim - eye, view);
		view.z = 0.0f;
		if (armed)
		{
			float heat = 0.0f;
			ReadEntityFloat(vehicle, "m_flVehicleHeat", heat);
			const float distance = Dist2D(origin, targetAt);
			trace_t hit;
			Ray_t ray;
			ray.Init(eye, EntityCenter(target));
			CTraceFilterWorldOnly filter;
			enginetrace->TraceRay(ray, MASK_SHOT, &filter, &hit);
			bool lineClear = hit.fraction > 0.97f;
			if (lineClear)
			{
				// Buildings and vehicles in between: hold fire (world-only trace missed them).
				FireLineFilter entities(m_pEdict, vehicle, target);
				enginetrace->TraceRay(ray, MASK_SHOT, &entities, &hit);
				lineClear = hit.fraction > 0.97f || (target->GetUnknown() != nullptr && hit.m_pEnt == target->GetUnknown()->GetBaseEntity());
			}
			if (lineClear)
				v.fightSight = now;
			if (lineClear && now - v.aimSince > 1.2f && heat < 80.0f)
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
				const char* why = "fallback";
				if (v.weaponsKnown)
				{
					// User 14.11.: MG against infantry, the cannon there only once every machine gun
					// of the group is out of magazine and reserve; cannon/missiles against vehicles
					// and buildings. Buttons that would also fire a cannon are not used on infantry.
					if (infantry)
					{
						if (distance < 3000.0f && v.weaponMgPure != 0)
						{
							buttons |= v.weaponMgPure;
							why = "mg";
						}
						else if (v.weaponMgMixed != 0)
							why = "mg-mixed"; // user: never fire a shared MG/cannon button at infantry
						else if (distance < 3000.0f && !v.weaponMgLeft && v.weaponHeavyInf != 0)
						{
							buttons |= v.weaponHeavyInf;
							why = "mg-empty";
						}
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
					if (distance < 3000.0f)
						buttons |= kMachineGun;
					if (distance < 1500.0f)
						buttons |= kCannon; // splash
				}
				else if (distance < 4000.0f)
					buttons |= kCannon;
				if (now >= v.nextAimLog)
				{
					v.nextAimLog = now + 5.0f;
					RCBOT_DIAG("[RCBOT2] vehicle fire userid=%d target=%d infantry=%d dist=%.0f buttons=%s%s why=%s lead=%.0f weapons=%s\n",
						userId, targetIndex, infantry ? 1 : 0, distance,
						(buttons & IN_ATTACK) ? "a1" : "", (buttons & IN_ATTACK2) ? "a2" : "", why,
						(aim - centre).Length(), v.weaponsKnown ? v.weaponSummary : "unknown");
				}
			}
		}
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
			"target=%d thp=%.0f btn=0x%x ram=%d billnye=%d route=%zu/%zu\n",
			userId, v.vehicleIndex, chassis, origin.x, origin.y, origin.z, heading, speed,
			driveTo.x, driveTo.y, goalDistance, desired, chosen, err, forward, side,
			found ? 1 : 0, headingSafe ? 1 : 0,
			target != nullptr ? engine->IndexOfEdict(target) : -1, targetHealth, buttons,
			ram ? 1 : 0, v.billNye ? 1 : 0, onRoute ? v.routeIndex : 0, v.route.size());
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
	RCBOT_DIAG("[RCBOT2] cv-drive start userid=%d cv=%d from=(%.0f %.0f %.0f) goal=(%.0f %.0f %.0f)\n",
		engine->GetPlayerUserId(m_pEdict), v.vehicleIndex, v.lastVehicleOrigin.x, v.lastVehicleOrigin.y,
		v.lastVehicleOrigin.z, goal.x, goal.y, goal.z);
	return true;
}

bool CBotEmpires::handleEmpiresVehicleRepairRequest()
{
	if (gpGlobals == nullptr || engine == nullptr || !isEmpiresEngineerClass())
		return false;
	const int team = getTeam();
	edict_t* hurt = RepairRequestVehicle(team);
	if (hurt == nullptr)
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
		*hp = std::max(1, static_cast<int>(before * factor));
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
					sides[k] = std::max(1.0f, std::floor(sides[k] * sideFactors[k]));
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

void EmpiresVehicleLevelReset()
{
	for (RepairRequest& r : s_repairRequest)
		r = RepairRequest();
	g_pushZones.clear();
	g_pushZonesAt = -1.0f;
}
