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
#include <vector>

bool EmpiresTeamResources(int team, float& resources);
bool EmpiresModifyTeamResources(int team, float delta);
bool EmpiresBinaryOffsetsTrusted();
void ApplyDistinctBotName(edict_t* pEdict, const char* szName, int team, int botClass); // bot.cpp

// Turret on the commander's order: "Attack location" (m_iCommandType 2) to an EngiBot. The order
// consumer walks it there; on arrival it places an MG turret facing the enemy and builds it up.
struct EngibotTurretJob
{
	int stage = 0; // 0 none, 1 wait for arrival / place, 2 placing, 3 building up, 4 done or given up
	Vector order;
	Vector stand;
	int tries = 0;
	int turret = 0;
	float next = 0.0f;
};

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
	EngibotTurretJob turret[kMaxEngibots];
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
		t.turret[kept] = t.turret[i];
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

namespace
{
// Requests from player_say, handled in the next GameFrame. Creating the fake client and sending
// the reply inside the event (that is, inside the "say" command and every plugin's say/event
// hooks) crashed a community server with SourceMod (jump to NULL on "!buildengibot").
struct EngibotRequest
{
	int userId;
	bool test;
};
EngibotRequest s_requests[8];
int s_requestCount = 0;

void HandleEngibotRequest(edict_t* speaker, int speakerId, bool test);
} // namespace

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
	for (int i = 0; i < s_requestCount; ++i)
		if (s_requests[i].userId == speakerId)
			return; // already queued this frame
	if (s_requestCount < static_cast<int>(sizeof(s_requests) / sizeof(s_requests[0])))
		s_requests[s_requestCount++] = {speakerId, test};
	RCBOT_DIAG("[RCBOT2] engibot request speaker=%d test=%d queued=%d\n", speakerId, test ? 1 : 0, s_requestCount);
}

namespace
{
void HandleEngibotRequest(edict_t* speaker, const int speakerId, const bool test)
{
	(void)test;
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
			t.turret[t.count] = EngibotTurretJob();
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
} // namespace

// New map: the fake clients stay connected across changelevel; EngiBots belong to one match.
extern int g_empiresTestHumanCommanderTeam;
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
	s_requestCount = 0;
	g_empiresTestHumanCommanderTeam = 0;
}

// Test only (rcbotd empires_test_engibot <speaker userid>): the speaker says /buildengibot in
// the real chat (player_say fires), as if a human commander typed it.
bool EmpiresTestEngibotChat(int speakerUserId, char prefix)
{
	edict_t* speaker = PlayerByUserId(speakerUserId);
	if (speaker == nullptr || helpers == nullptr)
		return false;
	s_testSpeaker = speakerUserId;
	char command[40];
	std::snprintf(command, sizeof(command), "say %cbuildengibot\n", prefix == '!' ? '!' : '/');
	helpers->ClientCommand(speaker, command);
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
	int order = 0;
	if (!readEmpiresIntProperty(m_pEdict, "m_iCommandType", order))
		order = 0;
	if (tickEngibotTurretOrder(t.turret[index], team, order))
		return true;
	if (now < t.nextTick[index])
		return true;
	t.nextTick[index] = now + 2.0f;

	edict_t* cv = TeamCommanderVehicle(team);
	// A standing commander order: work only around the order spot, else the order's leash (500)
	// and the base pull it back and forth (expert review 15.11.).
	const bool atOrder = order != 0 && m_bEmpiresCommanderMoveRouteIssued;
	const Vector anchor = atOrder ? m_vEmpiresCommanderMoveTarget : (cv != nullptr ? CBotGlobals::entityOrigin(cv) : t.home[index]);
	const float leash = atOrder ? 450.0f : std::max(500.0f, rcbot_empires_engibot_leash.GetFloat());
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
	// A standing commander order (attack location, guard) keeps it where the commander sent it.
	if (order == 0 && home > leash * 0.6f && (m_pSchedules == nullptr || m_pSchedules->isEmpty()))
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

namespace
{
bool GroundUnder(const Vector& p, Vector& ground)
{
	if (enginetrace == nullptr)
		return false;
	CTraceFilterWorldOnly filter;
	trace_t hit;
	Ray_t ray;
	ray.Init(p + Vector(0, 0, 60), p - Vector(0, 0, 200));
	enginetrace->TraceRay(ray, MASK_PLAYERSOLID_BRUSHONLY, &filter, &hit);
	if (hit.fraction >= 1.0f || hit.startsolid)
		return false;
	ground = hit.endpos;
	return true;
}

} // namespace

bool EmpiresFrontYaw(const Vector& at, int team, float& yaw); // bot_empires_commander_economy.cpp

// Commander "Attack location" to an EngiBot = build a turret there (community: turrets only on
// the commander's order). Returns true while the turret work owns the bot.
bool CBotEmpires::tickEngibotTurretOrder(EngibotTurretJob& job, const int team, const int order)
{
	const float now = gpGlobals->curtime;
	const int userId = engine->GetPlayerUserId(m_pEdict);
	if (order != 2 || m_iEmpiresCommanderOrderType != 2 || !m_bEmpiresCommanderMoveRouteIssued)
	{
		if (job.stage != 0 && job.stage != 4)
			RCBOT_DIAG("[RCBOT2] engibot turret end userid=%d reason=order-changed order=%d stage=%d\n", userId, order, job.stage);
		if (order != 2)
			job = EngibotTurretJob();
		return job.stage == 2 && isEmpiresEngineerBuildActionActive();
	}
	const Vector target = m_vEmpiresCommanderMoveTarget;
	if (job.stage == 0 || (job.order - target).Length2D() > 200.0f)
	{
		job = EngibotTurretJob();
		job.stage = 1;
		job.order = target;
		RCBOT_DIAG("[RCBOT2] engibot turret order userid=%d target=(%.0f %.0f %.0f) dist=%.0f\n", userId, target.x, target.y,
			target.z, (getOrigin() - target).Length2D());
	}
	char name[64];
	std::snprintf(name, sizeof(name), "%s", m_pPlayerInfo != nullptr && m_pPlayerInfo->GetName() != nullptr ? m_pPlayerInfo->GetName() : "EngiBot");
	char text[128];
	// Message on the screen of the team's human commander (whoever gave the order).
	auto notify = [this, team](const char* message)
	{
		for (int i = 1; i <= gpGlobals->maxClients; ++i)
		{
			edict_t* p = engine->PEntityOfEntIndex(i);
			IPlayerInfo* info = p != nullptr && !p->IsFree() && playerinfomanager != nullptr ? playerinfomanager->GetPlayerInfo(p) : nullptr;
			int commander = 0;
			if (info != nullptr && info->IsConnected() && !info->IsFakeClient() && CClassInterface::getTeam(p) == team &&
				readEmpiresEntityIntProperty(p, "m_bCommander", commander) && commander != 0)
				RCBotPluginMeta::HudTextMessage(p, message);
		}
	};
	if (job.stage == 1)
	{
		if (m_bEmpiresCommanderMoveFailed)
		{
			job.stage = 4;
			RCBOT_DIAG("[RCBOT2] engibot turret failed userid=%d reason=unreachable\n", userId);
			std::snprintf(text, sizeof(text), "%s: cannot reach that spot for a turret", name);
			notify(text);
			return false;
		}
		if (!m_bEmpiresCommanderMoveFinished || (getOrigin() - target).Length2D() > 250.0f)
			return false; // the order consumer walks it there
		if (now < job.next)
			return true; // between tries: no base repair in between (expert review 15.11.)
		if (isEmpiresEngineerBuildActionActive())
			return true;
		int built = -1, allotted = -1;
		if (readEmpiresDebugIntProperty(m_pEdict, "m_iEngineerTurretsBuilt", built) &&
			readEmpiresDebugIntProperty(m_pEdict, "m_iEngineerTurretsAllotted", allotted) && allotted >= 0 && built >= allotted)
		{
			job.stage = 4;
			RCBOT_DIAG("[RCBOT2] engibot turret failed userid=%d reason=limit built=%d allotted=%d\n", userId, built, allotted);
			std::snprintf(text, sizeof(text), "%s: turret limit reached (%d/%d)", name, built, allotted);
			notify(text);
			return false;
		}
		// Face the enemy: the front direction at the spot, else the enemy CV, else away from the own CV.
		float yaw = 0.0f;
		const Vector origin = getOrigin();
		if (!EmpiresFrontYaw(origin, team, yaw))
		{
			edict_t* enemyCv = TeamCommanderVehicle(team == 2 ? 3 : 2);
			edict_t* ownCv = TeamCommanderVehicle(team);
			const Vector to = enemyCv != nullptr ? CBotGlobals::entityOrigin(enemyCv) - origin :
				(ownCv != nullptr ? origin - CBotGlobals::entityOrigin(ownCv) : Vector(1, 0, 0));
			yaw = std::atan2(to.y, to.x) * 180.0f / 3.14159265f;
		}
		static const float kTurn[] = {0.0f, 45.0f, -45.0f, 90.0f, -90.0f, 135.0f, -135.0f, 180.0f};
		yaw += kTurn[job.tries % 8];
		const float r = yaw * 3.14159265f / 180.0f;
		Vector place = origin + Vector(std::cos(r), std::sin(r), 0.0f) * 60.0f;
		Vector ground;
		if (GroundUnder(place, ground))
			place = ground;
		cancelNavigationAttempt("engibot-turret-place");
		if (requestEmpiresEngineerBuildOnce(-1, &place, false))
		{
			job.stage = 2;
			job.stand = origin;
			RCBOT_DIAG("[RCBOT2] engibot turret place userid=%d try=%d yaw=%.0f target=(%.0f %.0f %.0f)\n", userId, job.tries, yaw,
				place.x, place.y, place.z);
			return true;
		}
		job.next = now + 1.0f;
		if (++job.tries >= 8)
		{
			job.stage = 4;
			RCBOT_DIAG("[RCBOT2] engibot turret failed userid=%d reason=build-rejected\n", userId);
			std::snprintf(text, sizeof(text), "%s: no room for a turret here", name);
			notify(text);
		}
		return false;
	}
	if (job.stage == 2)
	{
		if (isEmpiresEngineerBuildActionActive())
			return true;
		// The build action reports the new building; the distance search is only the fallback (an
		// older damaged turret nearby would be taken first, expert review 15.11.).
		edict_t* turret = nullptr;
		if (m_iEmpiresEngineerBuildActionResultIndex > 0)
			if (edict_t* e = engine->PEntityOfEntIndex(m_iEmpiresEngineerBuildActionResultIndex);
				e != nullptr && !e->IsFree() && e->GetClassName() != nullptr && std::strstr(e->GetClassName(), "turret") != nullptr)
				turret = e;
		for (int i = gpGlobals->maxClients + 1; i < gpGlobals->maxEntities && turret == nullptr; ++i)
		{
			edict_t* e = engine->PEntityOfEntIndex(i);
			int health = -1, maxHealth = -1;
			if (e != nullptr && !e->IsFree() && e->GetClassName() != nullptr && std::strncmp(e->GetClassName(), "emp_building_", 13) == 0 &&
				std::strstr(e->GetClassName(), "turret") != nullptr && CClassInterface::getTeam(e) == team &&
				(CBotGlobals::entityOrigin(e) - job.stand).Length2D() < 250.0f &&
				readEmpiresEntityIntProperty(e, "m_iHealth", health) && readEmpiresEntityIntProperty(e, "m_iMaxHealth", maxHealth) &&
				health < maxHealth)
				turret = e;
		}
		RCBOT_DIAG("[RCBOT2] engibot turret %s userid=%d try=%d turret=%d\n", turret != nullptr ? "placed" : "not-placed", userId,
			job.tries, turret != nullptr ? engine->IndexOfEdict(turret) : -1);
		if (turret == nullptr)
		{
			job.stage = ++job.tries >= 8 ? 4 : 1;
			job.next = now + 1.0f;
			if (job.stage == 4)
			{
				std::snprintf(text, sizeof(text), "%s: no room for a turret here", name);
				notify(text);
			}
			return false;
		}
		job.turret = engine->IndexOfEdict(turret);
		job.stage = 3;
		job.next = 0.0f;
		std::snprintf(text, sizeof(text), "%s: building a turret", name);
		notify(text);
	}
	if (job.stage == 3)
	{
		edict_t* turret = engine->PEntityOfEntIndex(job.turret);
		int health = -1, maxHealth = -1;
		const bool alive = turret != nullptr && !turret->IsFree() && turret->GetClassName() != nullptr &&
			std::strstr(turret->GetClassName(), "turret") != nullptr && CClassInterface::getTeam(turret) == team &&
			readEmpiresEntityIntProperty(turret, "m_iHealth", health) &&
			readEmpiresEntityIntProperty(turret, "m_iMaxHealth", maxHealth) && maxHealth > 0;
		if (!alive || health >= maxHealth)
		{
			job.stage = 4;
			RCBOT_DIAG("[RCBOT2] engibot turret %s userid=%d turret=%d health=%d/%d\n", alive ? "complete" : "gone", userId, job.turret,
				health, maxHealth);
			if (alive)
			{
				std::snprintf(text, sizeof(text), "%s: turret ready", name);
				notify(text);
			}
			return false;
		}
		if (now >= job.next)
		{
			job.next = now + 2.0f;
			if (requestEmpiresBuildAssignment(turret))
				return true;
			if (++job.tries >= 16)
			{
				job.stage = 4;
				RCBOT_DIAG("[RCBOT2] engibot turret unfinished userid=%d turret=%d health=%d/%d reason=assign-rejected\n", userId,
					job.turret, health, maxHealth);
				return false;
			}
		}
		return true; // holds the bot until the turret is full or given up
	}
	return false;
}

// Every 10 s from the plugin frame: drop EngiBots that left or switched team, and log each
// one's distance to its anchor (diagnosis).
void EmpiresEngibotFrame()
{
	// Chat requests from the last frames, now outside any event/command hook.
	const int requests = s_requestCount;
	s_requestCount = 0;
	for (int i = 0; i < requests && engine != nullptr && gpGlobals != nullptr; ++i)
		if (edict_t* speaker = PlayerByUserId(s_requests[i].userId); speaker != nullptr)
			HandleEngibotRequest(speaker, s_requests[i].userId, s_requests[i].test);
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
	CBotEmpires::empiresHelpHumanCommander();
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

// Test only (rcbotd empires_test_help_human <team>): the team's seated bot commander counts as human.
int g_empiresTestHumanCommanderTeam = 0;

void CBotEmpires::empiresHelpHumanCommander()
{
	const int limit = rcbot_empires_engineer_help_human.GetInt();
	if (limit <= 0 || engine == nullptr || gpGlobals == nullptr || !CBotGlobals::IsMapRunning() ||
		CBotGlobals::getCurrentMod() == nullptr || CBotGlobals::getCurrentMod()->getBotType() != BOTTYPE_EMPIRES)
		return;
	constexpr float kHelpRadius = 3000.0f; // around the team CV
	for (int team = 2; team <= 3; ++team)
	{
		std::vector<CBotEmpires*> engineers;
		CBotEmpires* reader = nullptr;
		for (int slot = 0; slot < RCBOT_MAXPLAYERS; ++slot)
		{
			CBot* bot = CBots::getBot(slot);
			if (bot == nullptr || !bot->inUse() || bot->getEdict() == nullptr || bot->isControlledTestOnly() ||
				CClassInterface::getTeam(bot->getEdict()) != team)
				continue;
			CBotEmpires* e = static_cast<CBotEmpires*>(bot);
			reader = e;
			if (e->isAlive() && e->isEmpiresEngineerClass() && !EmpiresIsEngibot(e->getEdict()))
				engineers.push_back(e);
		}
		if (reader == nullptr || engineers.empty())
			continue;
		// Only while a human of this team is the commander.
		bool human = false;
		for (int i = 1; i <= gpGlobals->maxClients && !human; ++i)
		{
			edict_t* p = engine->PEntityOfEntIndex(i);
			IPlayerInfo* info = p != nullptr && !p->IsFree() && playerinfomanager != nullptr ? playerinfomanager->GetPlayerInfo(p) : nullptr;
			int commander = 0;
			human = info != nullptr && info->IsConnected() && (!info->IsFakeClient() || g_empiresTestHumanCommanderTeam == team) &&
				CClassInterface::getTeam(p) == team &&
				reader->readEmpiresEntityIntProperty(p, "m_bCommander", commander) && commander != 0;
		}
		if (!human)
			continue;
		edict_t* cv = nullptr;
		for (int i = gpGlobals->maxClients + 1; i < gpGlobals->maxEntities && cv == nullptr; ++i)
		{
			edict_t* e = engine->PEntityOfEntIndex(i);
			if (e != nullptr && !e->IsFree() && e->GetClassName() != nullptr &&
				std::strcmp(e->GetClassName(), team == 2 ? "emp_nf_commander" : "emp_imp_commander") == 0)
				cv = e;
		}
		if (cv == nullptr)
			continue;
		const Vector cvOrigin = CBotGlobals::entityOrigin(cv);
		int busy = 0;
		std::vector<CBotEmpires*> free;
		for (CBotEmpires* e : engineers)
		{
			int order = 0;
			int seated = 0;
			if (e->m_iBuildAssignStage != 0 || e->m_iTurretRefinery != 0)
				++busy;
			else if ((e->readEmpiresEntityIntProperty(e->getEdict(), "m_bCommander", seated) && seated != 0) ||
				e->empiresVehicleOwnsBot() || e->isEmpiresVehicleActionActive())
				continue; // the seated commander, drivers and passengers stay where they are
			else if (!e->readEmpiresIntProperty(e->getEdict(), "m_iCommandType", order) || order == 0)
				free.push_back(e); // a commander order of the human goes first
		}
		if (busy >= limit || free.empty())
			continue;
		// The nearest unfinished or damaged own building to the CV that nobody works on yet.
		edict_t* best = nullptr;
		float bestDist = kHelpRadius;
		int bestHealth = 0, bestMax = 0;
		for (int i = gpGlobals->maxClients + 1; i < gpGlobals->maxEntities; ++i)
		{
			edict_t* b = engine->PEntityOfEntIndex(i);
			if (b == nullptr || b->IsFree() || b->GetClassName() == nullptr ||
				std::strncmp(b->GetClassName(), "emp_building_", 13) != 0 ||
				std::strcmp(b->GetClassName(), "emp_building_vehicle_physbox") == 0 || CClassInterface::getTeam(b) != team)
				continue;
			int health = -1, maxHealth = -1;
			if (!reader->readEmpiresEntityIntProperty(b, "m_iHealth", health) ||
				!reader->readEmpiresEntityIntProperty(b, "m_iMaxHealth", maxHealth) || health <= 0 || health >= maxHealth)
				continue;
			const float d = (CBotGlobals::entityOrigin(b) - cvOrigin).Length();
			if (d >= bestDist)
				continue;
			bool taken = false;
			for (CBotEmpires* e : engineers)
				taken = taken || (e->m_iBuildAssignStage != 0 && e->m_hBuildAssignBuilding.get() == b);
			if (taken)
				continue;
			best = b;
			bestDist = d;
			bestHealth = health;
			bestMax = maxHealth;
		}
		if (best == nullptr)
			continue;
		std::sort(free.begin(), free.end(), [best](CBotEmpires* a, CBotEmpires* b) { return a->distanceFrom(best) < b->distanceFrom(best); });
		const bool started = free.front()->requestEmpiresBuildAssignment(best);
		RCBOT_DIAG("[RCBOT2] engineer-help team=%d building=%d class=%s health=%d/%d engineer=%d busy=%d/%d started=%d\n",
			team, engine->IndexOfEdict(best), best->GetClassName(), bestHealth, bestMax,
			engine->GetPlayerUserId(free.front()->getEdict()), busy, limit, started ? 1 : 0);
	}
}
