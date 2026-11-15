// Community request (2026-11-15): the commander types /buildengibot in chat and gets an
// engineer bot that stays in the main base and builds/repairs there. It costs team resources
// (rcbot_empires_engibot_cost) and each team has a limit (rcbot_empires_engibot_limit).
// Turrets only on the commander's order. Step A: chat command, purchase, limit, cleanup.
// Resources: CEmpTeam+0x3b4 ledger, +0x30 float (RE buildings.md "Changing team resources").
#include "bot.h"
#include "bot_cvars.h"
#include "bot_empires.h"
#include "bot_empires_native_placement.h"
#include "bot_getprop.h"
#include "bot_globals.h"
#include "bot_plugin_meta.h"
#include "bot_navmesh_api.h"
#include "bot_schedule.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>

bool EmpiresTeamResources(int team, float& resources);
bool EmpiresModifyTeamResources(int team, float delta);
bool EmpiresBinaryOffsetsTrusted();
void ApplyDistinctBotName(edict_t* pEdict, const char* szName, int team, int botClass); // bot.cpp

namespace
{
constexpr int kMaxEngibots = 8; // per team, upper bound of the cvar

struct EngibotTeam
{
	int userIds[kMaxEngibots];
	int claim[kMaxEngibots] = {};     // building it repairs/builds (entity index), 0 = none
	float claimUntil[kMaxEngibots] = {};
	float nextTick[kMaxEngibots] = {};
	bool homeKnown[kMaxEngibots] = {};
	Vector home[kMaxEngibots];        // first spawn, used when the team has no CV
	int count = 0;
};
EngibotTeam s_engibots[4];
int s_testSpeaker = -1;

bool StartsWithCommand(const char* text, const char* command)
{
	if (text == nullptr)
		return false;
	while (*text == ' ' || *text == '"')
		++text;
	if (*text != '/' && *text != '!')
		return false;
	++text;
	const std::size_t n = std::strlen(command);
	for (std::size_t i = 0; i < n; ++i)
		if (std::tolower(static_cast<unsigned char>(text[i])) != command[i])
			return false;
	return text[n] == '\0' || text[n] == ' ' || text[n] == '"' || text[n] == '\n';
}

edict_t* PlayerByUserId(int userId)
{
	for (int i = 1; i <= gpGlobals->maxClients; ++i)
	{
		edict_t* e = engine->PEntityOfEntIndex(i);
		if (e != nullptr && !e->IsFree() && engine->GetPlayerUserId(e) == userId)
			return e;
	}
	return nullptr;
}

int ConnectedClients()
{
	int n = 0;
	for (int i = 1; i <= gpGlobals->maxClients; ++i)
	{
		edict_t* e = engine->PEntityOfEntIndex(i);
		IPlayerInfo* info = e != nullptr && !e->IsFree() && playerinfomanager != nullptr ? playerinfomanager->GetPlayerInfo(e) : nullptr;
		if (info != nullptr && info->IsConnected())
			++n;
	}
	return n;
}

void Reply(edict_t* speaker, const char* text)
{
	RCBotPluginMeta::HudTextMessage(speaker, text);
}

// Drops entries whose player left or changed team (autobalance: kicked, the slot is free again).
void Prune(int team)
{
	EngibotTeam& t = s_engibots[team];
	int kept = 0;
	for (int i = 0; i < t.count; ++i)
	{
		edict_t* e = PlayerByUserId(t.userIds[i]);
		const int now = e != nullptr ? CClassInterface::getTeam(e) : -1;
		if (e != nullptr && now != team && now >= 2)
		{
			char command[32];
			std::snprintf(command, sizeof(command), "kickid %d\n", t.userIds[i]);
			engine->ServerCommand(command);
			RCBOT_DIAG("[RCBOT2] engibot kick userid=%d team=%d reason=team-change now=%d\n", t.userIds[i], team, now);
			continue;
		}
		if (e == nullptr)
		{
			RCBOT_DIAG("[RCBOT2] engibot gone userid=%d team=%d\n", t.userIds[i], team);
			continue;
		}
		t.userIds[kept] = t.userIds[i];
		t.claim[kept] = t.claim[i];
		t.claimUntil[kept] = t.claimUntil[i];
		t.nextTick[kept] = t.nextTick[i];
		t.homeKnown[kept] = t.homeKnown[i];
		t.home[kept] = t.home[i];
		++kept;
	}
	t.count = kept;
}
} // namespace

bool EmpiresIsEngibot(const edict_t* player)
{
	if (player == nullptr || engine == nullptr)
		return false;
	const int userId = engine->GetPlayerUserId(player);
	for (const EngibotTeam& t : s_engibots)
		for (int i = 0; i < t.count; ++i)
			if (t.userIds[i] == userId)
				return true;
	return false;
}

// player_say (bot_events.cpp). Only a human commander counts, a bot speaker only for the test command.
void EmpiresHandleEngibotChat(edict_t* speaker, const char* text)
{
	if (speaker == nullptr || engine == nullptr || gpGlobals == nullptr || !StartsWithCommand(text, "buildengibot"))
		return;
	const int speakerId = engine->GetPlayerUserId(speaker);
	const bool test = s_testSpeaker >= 0 && speakerId == s_testSpeaker;
	if (test)
		s_testSpeaker = -1;
	if ((!test && CBots::getBotPointer(speaker) != nullptr) || CBotGlobals::getCurrentMod() == nullptr ||
		CBotGlobals::getCurrentMod()->getBotType() != BOTTYPE_EMPIRES)
		return;
	const int team = CClassInterface::getTeam(speaker);
	const char* reason = nullptr;
	char reply[160];
	float before = -1.0f;
	const int limit = std::max(0, std::min(kMaxEngibots, rcbot_empires_engibot_limit.GetInt()));
	const float cost = std::max(0.0f, rcbot_empires_engibot_cost.GetFloat());
	EmpiresNativePlacement::ContextIdentity context;
	bool commander = false;
	if (team == 2 || team == 3)
		Prune(team);
	if (!rcbot_empires_engibot.GetBool())
		reason = "disabled";
	else if (team != 2 && team != 3)
		reason = "no-team";
	else if (!EmpiresBinaryOffsetsTrusted())
		reason = "binary";
	else if (!EmpiresNativePlacement::CaptureContext(engine->IndexOfEdict(speaker), context) ||
		!EmpiresNativePlacement::ReadCommanderMode(context, commander) || !commander)
		reason = "not-commander";
	else if (s_engibots[team].count >= limit)
		reason = "limit";
	else if (ConnectedClients() >= gpGlobals->maxClients)
		reason = "server-full";
	else if (!EmpiresTeamResources(team, before) || before < cost)
		reason = "money";

	if (reason == nullptr)
	{
		int slot = -1;
		char name[32];
		std::snprintf(name, sizeof(name), "EngiBot %s %d", team == 2 ? "NF" : "BE", s_engibots[team].count + 1);
		if (!CBots::createBot("3", team == 2 ? "2" : "3", name, &slot) || slot < 0 || CBots::getBot(slot) == nullptr ||
			CBots::getBot(slot)->getEdict() == nullptr)
			reason = "create-failed";
		else if (!EmpiresModifyTeamResources(team, -cost))
		{
			// Never hand out a free bot: take it back.
			char command[32];
			std::snprintf(command, sizeof(command), "kickid %d\n", engine->GetPlayerUserId(CBots::getBot(slot)->getEdict()));
			engine->ServerCommand(command);
			reason = "pay-failed";
		}
		else
		{
			EngibotTeam& t = s_engibots[team];
			const int userId = engine->GetPlayerUserId(CBots::getBot(slot)->getEdict());
			t.userIds[t.count] = userId;
			t.claim[t.count] = 0;
			t.claimUntil[t.count] = 0.0f;
			t.nextTick[t.count] = 0.0f;
			t.homeKnown[t.count] = false;
			++t.count;
			float after = -1.0f;
			EmpiresTeamResources(team, after);
			RCBOT_DIAG("[RCBOT2] engibot buy team=%d speaker=%d cost=%.0f res_before=%.0f res_after=%.0f slot=%d userid=%d count=%d/%d\n",
				team, speakerId, cost, before, after, slot, userId, t.count, limit);
			std::snprintf(reply, sizeof(reply), "EngiBot ready (%d/%d), -%.0f resources", t.count, limit, cost);
			Reply(speaker, reply);
			return;
		}
	}
	RCBOT_DIAG("[RCBOT2] engibot refuse team=%d speaker=%d reason=%s count=%d limit=%d cost=%.0f res=%.0f\n", team,
		speakerId, reason, team == 2 || team == 3 ? s_engibots[team].count : 0, limit, cost, before);
	if (std::strcmp(reason, "limit") == 0)
		std::snprintf(reply, sizeof(reply), "EngiBot limit reached (%d/%d)", limit, limit);
	else if (std::strcmp(reason, "money") == 0)
		std::snprintf(reply, sizeof(reply), "Not enough resources for an EngiBot (%.0f needed)", cost);
	else if (std::strcmp(reason, "not-commander") == 0)
		std::snprintf(reply, sizeof(reply), "Only the commander can build an EngiBot");
	else if (std::strcmp(reason, "server-full") == 0)
		std::snprintf(reply, sizeof(reply), "Server is full, no slot for an EngiBot");
	else if (std::strcmp(reason, "disabled") == 0)
		std::snprintf(reply, sizeof(reply), "EngiBots are disabled on this server");
	else
		std::snprintf(reply, sizeof(reply), "Could not build an EngiBot (%s)", reason);
	Reply(speaker, reply);
}

// New map: the fake clients stay connected across changelevel; EngiBots belong to one match.
void EmpiresEngibotLevelReset()
{
	for (EngibotTeam& t : s_engibots)
	{
		for (int i = 0; i < t.count; ++i)
		{
			char command[32];
			std::snprintf(command, sizeof(command), "kickid %d\n", t.userIds[i]);
			if (engine != nullptr)
				engine->ServerCommand(command);
			RCBOT_DIAG("[RCBOT2] engibot kick userid=%d reason=level-reset\n", t.userIds[i]);
		}
		t.count = 0;
	}
	s_testSpeaker = -1;
}

// Test only (rcbotd empires_test_engibot <speaker userid>): the speaker says /buildengibot in
// the real chat (player_say fires), as if a human commander typed it.
bool EmpiresTestEngibotChat(int speakerUserId)
{
	edict_t* speaker = PlayerByUserId(speakerUserId);
	if (speaker == nullptr || helpers == nullptr)
		return false;
	s_testSpeaker = speakerUserId;
	helpers->ClientCommand(speaker, "say /buildengibot\n");
	return true;
}

namespace
{
bool Entry(int userId, int& team, int& index)
{
	for (team = 0; team < 4; ++team)
		for (index = 0; index < s_engibots[team].count; ++index)
			if (s_engibots[team].userIds[index] == userId)
				return true;
	return false;
}

edict_t* TeamCommanderVehicle(int team)
{
	const char* cvClass = team == 2 ? "emp_nf_commander" : "emp_imp_commander";
	for (int i = gpGlobals->maxClients + 1; i < gpGlobals->maxEntities; ++i)
	{
		edict_t* e = engine->PEntityOfEntIndex(i);
		if (e != nullptr && !e->IsFree() && e->GetClassName() != nullptr && std::strcmp(e->GetClassName(), cvClass) == 0)
			return e;
	}
	return nullptr;
}
} // namespace

// Called from the idle branch of the Empires task dispatch instead of capture, squad, roam and
// refinery turrets: an EngiBot works only inside the leash around its base (team CV, else its
// first spawn). It takes the nearest damaged or unfinished own building there (claim: one
// EngiBot per building) through the normal build assignment, else walks back home.
bool CBotEmpires::tickEmpiresEngibot()
{
	if (engine == nullptr || gpGlobals == nullptr || m_pEdict == nullptr || !isAlive())
		return false;
	int team = 0, index = 0;
	if (!Entry(engine->GetPlayerUserId(m_pEdict), team, index))
		return false;
	EngibotTeam& t = s_engibots[team];
	const float now = gpGlobals->curtime;
	const int userId = t.userIds[index];
	if (!t.homeKnown[index])
	{
		t.home[index] = getOrigin();
		t.homeKnown[index] = true;
	}
	if (m_iBuildAssignStage != 0)
	{
		t.claimUntil[index] = now + 3.0f;
		return true; // the assignment drives the bot
	}
	if (now < t.nextTick[index])
		return true;
	t.nextTick[index] = now + 2.0f;

	edict_t* cv = TeamCommanderVehicle(team);
	const Vector anchor = cv != nullptr ? CBotGlobals::entityOrigin(cv) : t.home[index];
	const float leash = std::max(500.0f, rcbot_empires_engibot_leash.GetFloat());
	const Vector origin = getOrigin();

	edict_t* best = nullptr;
	float bestDist = 1.0e9f;
	int bestHealth = 0, bestMax = 0;
	for (int i = gpGlobals->maxClients + 1; i < gpGlobals->maxEntities; ++i)
	{
		edict_t* e = engine->PEntityOfEntIndex(i);
		if (e == nullptr || e->IsFree() || e->GetClassName() == nullptr || std::strncmp(e->GetClassName(), "emp_building", 12) != 0 ||
			CClassInterface::getTeam(e) != team)
			continue;
		const Vector at = CBotGlobals::entityOrigin(e);
		if ((at - anchor).Length2D() > leash)
			continue;
		int health = -1, maxHealth = -1;
		if (!readEmpiresEntityIntProperty(e, "m_iHealth", health) || !readEmpiresEntityIntProperty(e, "m_iMaxHealth", maxHealth) ||
			maxHealth <= 0 || health < 0 || health >= maxHealth)
			continue;
		bool taken = false;
		for (int k = 0; k < t.count && !taken; ++k)
			taken = k != index && t.claim[k] == i && now <= t.claimUntil[k];
		const float d = (at - origin).Length2D();
		if (taken || d >= bestDist)
			continue;
		best = e;
		bestDist = d;
		bestHealth = health;
		bestMax = maxHealth;
	}
	if (best != nullptr)
	{
		const int bi = engine->IndexOfEdict(best);
		const bool started = requestEmpiresBuildAssignment(best);
		if (started || t.claim[index] != bi)
			RCBOT_DIAG("[RCBOT2] engibot repair userid=%d target=%d class=%s health=%d/%d dist=%.0f started=%d\n", userId, bi,
				best->GetClassName(), bestHealth, bestMax, bestDist, started ? 1 : 0);
		t.claim[index] = bi;
		t.claimUntil[index] = now + 3.0f;
		if (started)
			return true;
	}
	else
		t.claim[index] = 0;

	const float home = (origin - anchor).Length2D();
	if (home > leash * 0.6f && (m_pSchedules == nullptr || m_pSchedules->isEmpty()))
	{
		// Wait 900 out, each EngiBot on its own side: standing next to the CV blocked the
		// commander's building spots (run 20261115-030313: barracks 36/36 invalid, both at 328).
		Vector toBot = origin - anchor;
		toBot.z = 0.0f;
		Vector dir = toBot.Length() > 1.0f ? toBot.Normalized() : Vector(1, 0, 0);
		const float turn = 1.2f * static_cast<float>(index);
		dir = Vector(dir.x * std::cos(turn) - dir.y * std::sin(turn), dir.x * std::sin(turn) + dir.y * std::cos(turn), 0.0f);
		Vector spot = anchor + dir * 900.0f;
		Vector onMesh;
		if (RCBotNavMesh::ClosestPointOnMesh(spot, 400.0f, onMesh))
			spot = onMesh;
		if (m_bEmpiresNavGoalActive)
			endEmpiresNavmeshGoal("drop", "engibot-home", 0.0f);
		if (!requestNavigation(spot, RCBotNavigation::NavigationPolicy::NAVMESH_ONLY, 0))
			setMoveTo(spot);
		t.nextTick[index] = now + 5.0f;
		RCBOT_DIAG("[RCBOT2] engibot home userid=%d dist=%.0f anchor=%s\n", userId, home, cv != nullptr ? "cv" : "spawn");
	}
	return true;
}

// Every 10 s from the plugin frame: drop EngiBots that left or switched team, and log each
// one's distance to its anchor (diagnosis).
void EmpiresEngibotFrame()
{
	static float s_next = 0.0f;
	if (gpGlobals != nullptr && s_next - gpGlobals->curtime > 10.0f)
		s_next = 0.0f; // new map: curtime restarted (expert 15.11.)
	if (gpGlobals == nullptr || engine == nullptr || gpGlobals->curtime < s_next)
		return;
	s_next = gpGlobals->curtime + 10.0f;
	// Bots created without team/class are named "Bot <n>"; once they play, name them after both
	// ("NF Rifleman 2") so they can be told apart (user 2026-11-15).
	for (int slot = 0; slot < RCBOT_MAXPLAYERS; ++slot)
	{
		CBot* bot = CBots::getBot(slot);
		if (CBotGlobals::IsMapRunning() && bot != nullptr && bot->inUse() && bot->getEdict() != nullptr && !bot->isControlledTestOnly() &&
			CBotGlobals::getCurrentMod() != nullptr && CBotGlobals::getCurrentMod()->getBotType() == BOTTYPE_EMPIRES)
			static_cast<CBotEmpires*>(bot)->applyEmpiresTeamClassName();
	}
	for (int team = 2; team <= 3; ++team)
	{
		Prune(team);
		const EngibotTeam& t = s_engibots[team];
		edict_t* cv = t.count > 0 ? TeamCommanderVehicle(team) : nullptr;
		for (int i = 0; i < t.count; ++i)
		{
			edict_t* e = PlayerByUserId(t.userIds[i]);
			if (e == nullptr || (cv == nullptr && !t.homeKnown[i]))
				continue;
			const Vector anchor = cv != nullptr ? CBotGlobals::entityOrigin(cv) : t.home[i];
			RCBOT_DIAG("[RCBOT2] engibot report userid=%d team=%d dist_anchor=%.0f claim=%d alive=%d\n", t.userIds[i], team,
				(CBotGlobals::entityOrigin(e) - anchor).Length2D(), t.claim[i], CBotGlobals::entityIsAlive(e) ? 1 : 0);
		}
	}
}

void CBotEmpires::applyEmpiresTeamClassName()
{
	// Fresh player info: during a map change the cached pointer may be stale.
	if (m_pEdict == nullptr || m_pEdict->IsFree() || playerinfomanager == nullptr || !CBotGlobals::IsMapRunning())
		return;
	IPlayerInfo* info = playerinfomanager->GetPlayerInfo(m_pEdict);
	if (info == nullptr || !info->IsConnected() || info->IsDead() || !isAlive())
		return;
	// Only names from ApplyDistinctBotName without a team: "Bot <n>" or "<Class> <n>".
	const char* name = info->GetName();
	static const char* const kPrefixes[] = {"Bot ", "Scout ", "Rifleman ", "Grenadier ", "Engineer "};
	bool generic = false;
	for (const char* prefix : kPrefixes)
	{
		const std::size_t n = std::strlen(prefix);
		generic = generic || (name != nullptr && std::strncmp(name, prefix, n) == 0 && name[n] >= '0' && name[n] <= '9');
	}
	if (!generic)
		return;
	const int team = getTeam();
	int classId = -1;
	bool desired = false, desiredKnown = false;
	const char* prop = nullptr;
	if ((team != 2 && team != 3) || !readEmpiresClass(m_pEdict, classId, desired, desiredKnown, prop) || classId < 0 || classId > 3)
		return;
	ApplyDistinctBotName(m_pEdict, nullptr, team, classId);
	RCBOT_DIAG("[RCBOT2] bot-name userid=%d from=%s team=%d class=%d\n", engine->GetPlayerUserId(m_pEdict), name, team, classId);
}
