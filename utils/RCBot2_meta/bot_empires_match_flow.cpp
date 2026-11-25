// Match-flow telemetry (MATCH_FLOW.md): a test bench, not bot behaviour. With rcbot_empires_debug_log 1
// (on in the dev harness, default 0) any bot's modThink drives one global scan per game second:
//   [RCBOT2] match-flow event=...   placed / accepted / build-start / finished / lost, research-vf, vehicle-bought /
//                                   vehicle-out / vehicle-front / vehicle-lost, each with t= (match seconds)
//   [RCBOT2] match-flow state ...   every 30 game seconds per team: tickets, resources, research, finished and
//                                   unfinished buildings (unfinished with age/idle seconds), living vehicles,
//                                   team-director phase.
// scripts/match_flow.py checks these lines against the limits in MATCH_FLOW.md (dev_team_match.sh).
#include "bot.h"
#include "bot_cvars.h"
#include "bot_empires.h"
#include "bot_empires_team_director.h"
#include "bot_globals.h"

#include <algorithm>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

extern IVEngineServer* engine;

bool EmpiresVehicleAlive(const CBotEmpires* bot, const edict_t* vehicle); // bot_empires_vehicle.cpp
bool EmpiresTeamResources(int team, float& resources); // bot_empires_commander_economy.cpp
bool EmpiresReadResearchState(int team, int& active, bool& paused, bool& building, bool& vfDone, bool& allDone);

namespace
{
constexpr float kScanSeconds = 1.0f;
constexpr float kStateSeconds = 30.0f;
constexpr float kOutDistance = 1000.0f;   // vehicle left the factory area
constexpr float kFrontDistance = 3000.0f; // within this of an enemy building or CV = front
constexpr float kFrontShare = 0.5f;       // or half the way from its factory to the enemy CV

enum Kind { REFINERY, RADAR, FACTORY, BARRACKS, ARMORY, REPAIR, KIND_COUNT };
const char* const kKindName[KIND_COUNT] = { "refinery", "radar", "factory", "barracks", "armory", "repair" };

int KindOf(const char* cls)
{
	if (std::strstr(cls, "_eng") != nullptr || std::strstr(cls, "panel") != nullptr || std::strstr(cls, "physbox") != nullptr)
		return -1;
	if (std::strstr(cls, "refinery") != nullptr) return REFINERY;
	if (std::strstr(cls, "radar") != nullptr) return RADAR;
	if (std::strstr(cls, "vehiclefactory") != nullptr) return FACTORY;
	if (std::strstr(cls, "barracks") != nullptr) return BARRACKS;
	if (std::strstr(cls, "armory") != nullptr) return ARMORY;
	if (std::strstr(cls, "repairstation") != nullptr) return REPAIR;
	return -1;
}

struct Building
{
	int serial = -1;
	int team = 0;
	int kind = -1;
	int health = 0;
	float placedAt = 0.0f;
	float lastProgress = 0.0f;
	float maxIdle = 0.0f;
	bool started = false;
	bool accepted = false;
	bool built = false;
	bool initial = false;
	bool seen = false;
};

struct Vehicle
{
	int serial = -1;
	int team = 0;
	int chassis = -1;
	float boughtAt = 0.0f;
	Vector from;
	bool out = false;
	bool front = false;
	bool seen = false;
};

struct Flow
{
	bool started = false;
	float startedAt = 0.0f;
	float nextScan = 0.0f;
	float nextState = 0.0f;
	bool vfResearch[4] = {};
	std::unordered_map<int, Building> buildings;
	std::unordered_map<int, Vehicle> vehicles;
	bool cvSeen[4] = {};
	Vector cvLast[4];
};
Flow s_flow;

const char* ResearchText(int team)
{
	int active = 0;
	bool paused = false, building = false, vfDone = false, allDone = false;
	if (!EmpiresReadResearchState(team, active, paused, building, vfDone, allDone))
		return "unknown";
	if (allDone)
		return "done";
	if (!building)
		return "none";
	if (active != 0)
		return paused ? "paused" : "active";
	return "idle";
}
} // namespace

void EmpiresMatchFlowLevelReset()
{
	s_flow = Flow();
}

void CBotEmpires::tickEmpiresMatchFlow()
{
	if (!rcbot_empires_debug_log.GetBool() || gpGlobals == nullptr || engine == nullptr)
		return;
	const float now = gpGlobals->curtime;
	Flow& flow = s_flow;
	if (flow.started && now < flow.nextScan)
		return;
	const bool first = !flow.started;
	if (first)
	{
		flow.started = true;
		flow.startedAt = now;
		flow.nextState = now + kStateSeconds;
		RCBOT_DIAG("[RCBOT2] match-flow event=start t=0 curtime=%.0f\n", now);
	}
	flow.nextScan = now + kScanSeconds;
	const float t = now - flow.startedAt;
	auto readInt = [this](edict_t* e, const char* prop, int fallback)
	{
		int value = fallback;
		return readEmpiresEntityIntProperty(e, prop, value) ? value : fallback;
	};

	for (auto& entry : flow.buildings)
		entry.second.seen = false;
	for (auto& entry : flow.vehicles)
		entry.second.seen = false;

	// Buildings an engineer (or the commander himself) has taken as a build order.
	std::vector<int> assigned;
	for (int slot = 0; slot < RCBOT_MAXPLAYERS; ++slot)
	{
		CBot* bot = CBots::getBot(slot);
		if (bot == nullptr || !bot->inUse() || bot->getEdict() == nullptr)
			continue;
		CBotEmpires* other = static_cast<CBotEmpires*>(bot);
		edict_t* target = other->m_iBuildAssignStage != 0 ? other->m_hBuildAssignBuilding.get() :
			(other->m_iCommanderBuildStage != 0 ? other->m_hCommanderBuildTarget.get() : nullptr);
		if (target != nullptr && !target->IsFree())
			assigned.push_back(engine->IndexOfEdict(target));
	}

	// Enemy positions for the front test: CV and any building, per team that owns them.
	std::vector<Vector> teamTargets[4];
	bool cvKnown[4] = {};
	Vector cv[4];
	for (int i = gpGlobals->maxClients + 1; i < gpGlobals->maxEntities; ++i)
	{
		edict_t* e = engine->PEntityOfEntIndex(i);
		if (e == nullptr || e->IsFree() || e->GetIServerEntity() == nullptr || e->GetClassName() == nullptr)
			continue;
		const char* cls = e->GetClassName();
		const bool nfCv = std::strcmp(cls, "emp_nf_commander") == 0;
		if (nfCv || std::strcmp(cls, "emp_imp_commander") == 0)
		{
			const int team = nfCv ? EMP_TEAM_NF : EMP_TEAM_BE;
			if (!cvKnown[team] && EmpiresVehicleAlive(this, e))
			{
				cvKnown[team] = true;
				cv[team] = CBotGlobals::entityOrigin(e);
				teamTargets[team].push_back(cv[team]);
			}
			continue;
		}
		if (std::strcmp(cls, "emp_vehicle") == 0)
		{
			const int team = readInt(e, "m_iTeamNum", -1);
			const int chassis = readInt(e, "m_iChassis", -1);
			if ((team != EMP_TEAM_NF && team != EMP_TEAM_BE) || (chassis >= 14 && chassis <= 16) || !EmpiresVehicleAlive(this, e))
				continue;
			Vehicle& v = flow.vehicles[i];
			const int serial = e->m_NetworkSerialNumber;
			if (v.serial != serial)
			{
				v = Vehicle();
				v.serial = serial;
				v.team = team;
				v.chassis = chassis;
				v.boughtAt = t;
				v.from = CBotGlobals::entityOrigin(e);
				RCBOT_DIAG("[RCBOT2] match-flow event=vehicle-bought t=%.0f team=%d ent=%d chassis=%d\n", t, team, i, chassis);
			}
			v.seen = true;
			continue;
		}
		if (std::strncmp(cls, "emp_building_", 13) != 0 || std::strstr(cls, "physbox") != nullptr ||
			std::strstr(cls, "panel") != nullptr)
			continue;
		int team = readInt(e, "m_iTeamNum", -1);
		if (team != EMP_TEAM_NF && team != EMP_TEAM_BE)
			team = std::strncmp(cls, "emp_building_nf_", 16) == 0 ? EMP_TEAM_NF :
				(std::strncmp(cls, "emp_building_imp_", 17) == 0 ? EMP_TEAM_BE : -1);
		const int hp = readInt(e, "m_iHealth", 0);
		if ((team != EMP_TEAM_NF && team != EMP_TEAM_BE) || hp <= 0)
			continue;
		teamTargets[team].push_back(CBotGlobals::entityOrigin(e));
		const int kind = KindOf(cls);
		if (kind < 0)
			continue;
		const int maxHp = readInt(e, "m_iMaxHealth", 0);
		bool built = false;
		if (!readEmpiresEntityBoolProperty(e, "m_bFullyBuilt", built))
			built = maxHp <= 0 || hp >= maxHp;
		built = built || (maxHp > 0 && hp >= maxHp); // see team director: map buildings may skip SetHealth
		Building& b = flow.buildings[i];
		const int serial = e->m_NetworkSerialNumber;
		if (b.serial != serial)
		{
			b = Building();
			b.serial = serial;
			b.team = team;
			b.kind = kind;
			b.health = hp;
			b.placedAt = b.lastProgress = t;
			b.built = built;
			b.initial = first;
			RCBOT_DIAG("[RCBOT2] match-flow event=placed t=%.0f team=%d kind=%s ent=%d hp=%d/%d initial=%d built=%d\n", t, team,
				kKindName[kind], i, hp, maxHp, first ? 1 : 0, built ? 1 : 0);
		}
		b.seen = true;
		if (b.built)
		{
			b.health = hp;
			continue;
		}
		if (!b.accepted && std::find(assigned.begin(), assigned.end(), i) != assigned.end())
		{
			b.accepted = true;
			RCBOT_DIAG("[RCBOT2] match-flow event=accepted t=%.0f team=%d kind=%s ent=%d wait=%.0f\n", t, team,
				kKindName[kind], i, t - b.placedAt);
		}
		if (hp > b.health)
		{
			b.lastProgress = t;
			if (!b.started)
			{
				b.started = true;
				b.accepted = true; // someone builds it, whether ordered or not
				RCBOT_DIAG("[RCBOT2] match-flow event=build-start t=%.0f team=%d kind=%s ent=%d wait=%.0f\n", t, team,
					kKindName[kind], i, t - b.placedAt);
			}
		}
		b.health = hp;
		if (t - b.lastProgress > b.maxIdle)
			b.maxIdle = t - b.lastProgress;
		if (built)
		{
			b.built = true;
			RCBOT_DIAG("[RCBOT2] match-flow event=finished t=%.0f team=%d kind=%s ent=%d age=%.0f max_idle=%.0f\n", t, team,
				kKindName[kind], i, t - b.placedAt, b.maxIdle);
		}
	}

	for (auto it = flow.buildings.begin(); it != flow.buildings.end();)
	{
		const Building& b = it->second;
		if (b.seen)
		{
			++it;
			continue;
		}
		RCBOT_DIAG("[RCBOT2] match-flow event=lost t=%.0f team=%d kind=%s ent=%d built=%d started=%d accepted=%d age=%.0f max_idle=%.0f\n",
			t, b.team, kKindName[b.kind], it->first, b.built ? 1 : 0, b.started ? 1 : 0, b.accepted ? 1 : 0, t - b.placedAt,
			b.maxIdle);
		it = flow.buildings.erase(it);
	}
	// A destroyed CV ends the match (game over, then the server changes map). Name it so the
	// evaluation does not read the map change as a harness fault (Palmbay 20261125-135810: NF CV drowned).
	for (int team = EMP_TEAM_NF; team <= EMP_TEAM_BE; ++team)
	{
		if (flow.cvSeen[team] && !cvKnown[team])
		{
			const Vector& at = flow.cvLast[team];
			const bool water = enginetrace != nullptr &&
				(enginetrace->GetPointContents(at + Vector(0, 0, 64.0f)) & MASK_WATER) != 0;
			RCBOT_DIAG("[RCBOT2] match-flow event=cv-lost t=%.0f team=%d at=(%.0f %.0f %.0f) water=%d\n", t, team,
				at.x, at.y, at.z, water ? 1 : 0);
		}
		flow.cvSeen[team] = cvKnown[team];
		if (cvKnown[team])
			flow.cvLast[team] = cv[team];
	}
	for (auto it = flow.vehicles.begin(); it != flow.vehicles.end();)
	{
		Vehicle& v = it->second;
		if (!v.seen)
		{
			RCBOT_DIAG("[RCBOT2] match-flow event=vehicle-lost t=%.0f team=%d ent=%d out=%d front=%d age=%.0f\n", t, v.team,
				it->first, v.out ? 1 : 0, v.front ? 1 : 0, t - v.boughtAt);
			it = flow.vehicles.erase(it);
			continue;
		}
		edict_t* e = engine->PEntityOfEntIndex(it->first);
		const Vector at = CBotGlobals::entityOrigin(e);
		if (!v.out && (at - v.from).Length2D() >= kOutDistance)
		{
			v.out = true;
			RCBOT_DIAG("[RCBOT2] match-flow event=vehicle-out t=%.0f team=%d ent=%d after=%.0f\n", t, v.team, it->first,
				t - v.boughtAt);
		}
		if (!v.front)
		{
			const int enemy = v.team == EMP_TEAM_NF ? EMP_TEAM_BE : EMP_TEAM_NF;
			const char* why = nullptr;
			for (const Vector& target : teamTargets[enemy])
				if ((target - at).Length2D() <= kFrontDistance)
					why = "near-enemy";
			if (why == nullptr && cvKnown[enemy])
			{
				const float total = (cv[enemy] - v.from).Length2D();
				if (total > 1.0f && total - (cv[enemy] - at).Length2D() >= total * kFrontShare)
					why = "half-way";
			}
			if (why != nullptr)
			{
				v.front = true;
				RCBOT_DIAG("[RCBOT2] match-flow event=vehicle-front t=%.0f team=%d ent=%d after=%.0f reason=%s\n", t, v.team,
					it->first, t - v.boughtAt, why);
			}
		}
		++it;
	}

	for (int team = EMP_TEAM_NF; team <= EMP_TEAM_BE; ++team)
	{
		int active = 0;
		bool paused = false, building = false, vfDone = false, allDone = false;
		if (!flow.vfResearch[team] && EmpiresReadResearchState(team, active, paused, building, vfDone, allDone) && vfDone)
		{
			flow.vfResearch[team] = true;
			RCBOT_DIAG("[RCBOT2] match-flow event=research-vf t=%.0f team=%d instant=%d\n", t, team, first ? 1 : 0);
		}
	}

	if (now < flow.nextState)
		return;
	flow.nextState += kStateSeconds;
	if (flow.nextState <= now)
		flow.nextState = now + kStateSeconds;
	for (int team = EMP_TEAM_NF; team <= EMP_TEAM_BE; ++team)
	{
		int built[KIND_COUNT] = {}, raw[KIND_COUNT] = {};
		std::string rawList;
		for (const auto& entry : flow.buildings)
		{
			const Building& b = entry.second;
			if (b.team != team)
				continue;
			if (b.built)
			{
				++built[b.kind];
				continue;
			}
			++raw[b.kind];
			char item[64];
			snprintf(item, sizeof(item), "%s%s#%d:%.0f/%.0f/%d", rawList.empty() ? "" : ",", kKindName[b.kind], entry.first,
				t - b.placedAt, t - b.lastProgress, b.accepted ? 1 : 0);
			rawList += item;
		}
		int vehicles = 0;
		for (const auto& entry : flow.vehicles)
			vehicles += entry.second.team == team ? 1 : 0;
		float resources = -1.0f;
		EmpiresTeamResources(team, resources);
		const EmpiresTeamDirector::Board* board = EmpiresTeamDirectorBoard(team);
		RCBOT_DIAG("[RCBOT2] match-flow state t=%.0f team=%d tickets=%.0f res=%.0f research=%s "
			"built=ref:%d,radar:%d,vf:%d,bar:%d,arm:%d,pad:%d raw=ref:%d,radar:%d,vf:%d,bar:%d,arm:%d,pad:%d "
			"rawlist=%s vehicles=%d phase=%s\n",
			t, team, board != nullptr && board->ticketsKnown ? board->tickets : -1.0f, resources, ResearchText(team),
			built[REFINERY], built[RADAR], built[FACTORY], built[BARRACKS], built[ARMORY], built[REPAIR],
			raw[REFINERY], raw[RADAR], raw[FACTORY], raw[BARRACKS], raw[ARMORY], raw[REPAIR],
			rawList.empty() ? "-" : rawList.c_str(), vehicles,
			board != nullptr ? EmpiresTeamDirector::PhaseName(board->phase) : "-");
	}
}
