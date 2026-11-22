// This is an open source non-commercial project. Dear PVS-Studio, please check it.
// PVS-Studio Static Code Analyzer for C, C++, C#, and Java: http://www.viva64.com
/**
 * vim: set ts=4 sw=4 tw=99 noet :
 * ======================================================
 * Metamod:Source Sample Plugin
 * Written by AlliedModders LLC.
 * ======================================================
 *
 * This software is provided 'as-is', without any express or implied warranty.
 * In no event will the authors be held liable for any damages arising from 
 * the use of this software.
 *
 * This sample plugin is public domain.
 */

#include <cstdio>
#include <vector>

#pragma push_macro("clamp") //Fix for C++17 [APG]RoboCop[CL]
#undef clamp
#include <algorithm>
#pragma pop_macro("clamp")

#include "bot_plugin_meta.h"

#include "filesystem.h"
#include "interface.h"
#include "IStaticPropMgr.h"

#ifdef __linux__
#include "shake.h"    //bir3yk
#endif

#include "ndebugoverlay.h"
#include "irecipientfilter.h"

#include "bot_cvars.h"
#include "bot_navmesh_api.h"
#include "bot_navmesh_provider.h"
#include "bot_local_explorer.h"
#include "bot_navmesh_poc.h"

#if defined(_WIN64) || defined(_WIN32)
	#include <ctime>
#endif

// for IServerTools
#include "bot.h"
#include "bot_configfile.h"
#include "bot_globals.h"
#include "bot_profile.h"
#include "bot_menu.h"
#include "bot_getprop.h"
#include "bot_event.h"
#include "bot_profiling.h"
#include "bot_wpt_dist.h"
#include "bot_squads.h"
#include "bot_accessclient.h"
#include "bot_weapons.h"
#include "bot_waypoint_visibility.h"
#include "bot_kv.h"
#include "bot_sigscan.h"
#include "bot_mods.h"
#include "bot_empires_telemetry.h"
#include "bot_empires_lawliet.h"

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wreorder"
#endif
#include "ientityinfo.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#include "tier0/icommandline.h"

#include "rcbot/logging.h"

#include <cstring>

#include <build_info.h>
#include <sourcehook.h>

void EmpiresRecordInfantryAmmoCall(edict_t* player);
void EmpiresEngibotFrame(); // bot_empires_engibot.cpp
bool EmpiresIsEngibot(const edict_t* player);
void EmpiresHandleEngibotChat(edict_t* speaker, const char* text); // bot_empires_engibot.cpp

#ifdef SM_EXT
#include "rcbot/entprops.h"

#ifdef RCBOT_VPROF_ENABLED
#include <tier0/vprof.h>
#endif // RCBOT_VPROF_ENABLED
#endif

// SourceHook SH_DECL_HOOK* macros use implicit false-to-int casts and uninitialized orig_ret for void hooks - [APG]RoboCop[CL]
SH_DECL_HOOK6(IServerGameDLL, LevelInit, SH_NOATTRIB, 0, bool, char const *, char const *, char const *, char const *, bool, bool);
SH_DECL_HOOK3_void(IServerGameDLL, ServerActivate, SH_NOATTRIB, 0, edict_t *, int, int);
SH_DECL_HOOK1_void(IServerGameDLL, GameFrame, SH_NOATTRIB, 0, bool);
SH_DECL_HOOK0_void(IServerGameDLL, LevelShutdown, SH_NOATTRIB, 0);
SH_DECL_HOOK2_void(IServerGameClients, ClientActive, SH_NOATTRIB, 0, edict_t *, bool);
SH_DECL_HOOK1_void(IServerGameClients, ClientDisconnect, SH_NOATTRIB, 0, edict_t *);
SH_DECL_HOOK2_void(IServerGameClients, ClientPutInServer, SH_NOATTRIB, 0, edict_t *, char const *);
SH_DECL_HOOK1_void(IServerGameClients, SetCommandClient, SH_NOATTRIB, 0, int);
SH_DECL_HOOK5(IServerGameClients, ClientConnect, SH_NOATTRIB, 0, bool, edict_t *, const char*, const char *, char *, int);
SH_DECL_HOOK2(IGameEventManager2, FireEvent, SH_NOATTRIB, 0, bool, IGameEvent *, bool);
SH_DECL_HOOK2(IVEngineServer, UserMessageBegin, SH_NOATTRIB, 0, bf_write *, IRecipientFilter *, int);
SH_DECL_HOOK0_void(IVEngineServer, MessageEnd, SH_NOATTRIB, 0);

#if SOURCE_ENGINE >= SE_ORANGEBOX
SH_DECL_HOOK2_void(IServerGameClients, NetworkIDValidated, SH_NOATTRIB, 0, const char *, const char *);
SH_DECL_HOOK2_void(IServerGameClients, ClientCommand, SH_NOATTRIB, 0, edict_t *, const CCommand &);
#else
SH_DECL_HOOK1_void(IServerGameClients, ClientCommand, SH_NOATTRIB, 0, edict_t *);
#endif

SH_DECL_MANUALHOOK2_void(MHook_PlayerRunCmd, 0, 0, 0, CUserCmd*, IMoveHelper*);

IServerGameDLL *server = nullptr;
IGameEventManager2 *gameevents = nullptr;
IServerPluginCallbacks *vsp_callbacks = nullptr;
ICvar *icvar = nullptr;
IVEngineServer *engine = nullptr;  // helper functions (messaging clients, loading content, making entities, running commands, etc)
IFileSystem *filesystem = nullptr;  // file I/O 
IGameEventManager2 *gameeventmanager = nullptr;
IGameEventManager *gameeventmanager1 = nullptr;  // game events interface
IPlayerInfoManager *playerinfomanager = nullptr;  // game dll interface to interact with players
IEntityInfoManager *entityinfomanager = nullptr; // transient entity telemetry interface
IServerPluginHelpers *helpers = nullptr;  // special 3rd party plugin helpers from the engine
IServerGameClients* gameclients = nullptr;
IEngineTrace *enginetrace = nullptr;
IEffects *g_pEffects = nullptr;
IBotManager *g_pBotManager = nullptr;
CGlobalVars *gpGlobals = nullptr;
IVDebugOverlay *debugoverlay = nullptr;
IServerGameEnts *servergameents = nullptr; // for accessing the server game entities
IServerGameDLL *servergamedll = nullptr;
IServerTools *servertools = nullptr;
IStaticPropMgrServer* staticpropmgr = nullptr;

RCBotPluginMeta g_RCBotPluginMeta;

PLUGIN_EXPOSE(RCBotPluginMeta, g_RCBotPluginMeta);

static ConVar rcbot2_ver_cvar("rcbot_ver", build_info::long_version, FCVAR_REPLICATED, "RCBot version");

namespace
{
	bool g_bEmpiresDeferredCommanderBot = false;

	bool readEmpiresBoolForDeferredBot(edict_t* pEdict, const char* propertyName, bool& value)
	{
		value = false;
		if (pEdict == nullptr || pEdict->IsFree() || propertyName == nullptr ||
			servergamedll == nullptr)
			return false;

		const ServerClass* playerClass = UTIL_FindServerClass("CEmpPlayer");
		if (playerClass == nullptr || playerClass->m_pTable == nullptr)
			return false;
		SendProp* prop = UTIL_FindSendProp(playerClass->m_pTable, propertyName);
		if (prop == nullptr || prop->GetType() != DPT_Int || prop->GetNumElements() != 1)
			return false;
		unsigned offset = 0;
		if (!UTIL_FindSendPropInfo(playerClass, propertyName, &offset) || offset == 0)
			return false;

		CClassInterfaceValue propertyValue("CEmpPlayer", propertyName, 0);
		propertyValue.findOffset();
		bool* currentValue = propertyValue.getBoolPointer(pEdict);
		if (currentValue == nullptr)
			return false;
		value = *currentValue;
		return true;
	}

	edict_t* findEmpiresCommanderInterface()
	{
		if (engine == nullptr || gpGlobals == nullptr || playerinfomanager == nullptr)
			return nullptr;

		for (int index = 1; index <= gpGlobals->maxClients; ++index)
		{
			edict_t* pEdict = engine->PEntityOfEntIndex(index);
			if (pEdict == nullptr || pEdict->IsFree())
				continue;
			IPlayerInfo* info = playerinfomanager->GetPlayerInfo(pEdict);
			if (info == nullptr || !info->IsConnected() || info->IsFakeClient() || info->IsHLTV())
				continue;

			bool inCommanderInterface = false;
			if (readEmpiresBoolForDeferredBot(pEdict, "m_bInCommanderInterface",
				inCommanderInterface) && inCommanderInterface)
				return pEdict;
		}

		return nullptr;
	}
}

CON_COMMAND(rcbot_empires_spawn_after_commander_f2,
	"arm one test-only Empires bot to be created after the human enters the Commander interface with F2")
{
	if (CBotGlobals::getCurrentMod() == nullptr ||
		CBotGlobals::getCurrentMod()->getBotType() != BOTTYPE_EMPIRES)
	{
		logger->Log(LogLevel::ERROR,
			"Empires deferred Commander bot spawn is available only in Empires");
		return;
	}

	if (CBots::getFirstNormalBot() != nullptr)
	{
		logger->Log(LogLevel::ERROR,
			"Empires deferred Commander bot spawn refused: a normal bot already exists");
		return;
	}

	// Keep the quota reconciler from creating unrelated bots both before and
	// after the one requested bot has been created. The flag is cleared on
	// level shutdown together with the deferred-spawn state.
	CBots::setEmpiresBootstrapTestMode(true);
	g_bEmpiresDeferredCommanderBot = true;
	logger->Log(LogLevel::INFO,
		"Empires deferred Commander bot spawn armed: no bot will be created until m_bInCommanderInterface=1 (F2)");
}

CON_COMMAND(rcbotd, "access the bot commands on a server")
{
	if (!engine->IsDedicatedServer() || !CBotGlobals::IsMapRunning())
	{
		logger->Log(LogLevel::ERROR, "Error, no map running or not dedicated server");
		return;
	}

	// shift args and call subcommand
	BotCommandArgs argList;
	for (unsigned i = 1; i <= static_cast<unsigned>(args.ArgC()); i++) {
		argList.emplace_back(args.Arg(static_cast<int>(i)));
	}
	const eBotCommandResult iResult = CBotGlobals::m_pCommands->execute(nullptr, argList);

	if (iResult == COMMAND_ACCESSED)
	{
		// ok
	}
	else if (iResult == COMMAND_REQUIRE_ACCESS)
	{
		logger->Log(LogLevel::ERROR, "You do not have access to this command");
	}
	else if (iResult == COMMAND_NOT_FOUND)
	{
		logger->Log(LogLevel::ERROR, "bot command not found");
	}
	else if (iResult == COMMAND_ERROR)
	{
		logger->Log(LogLevel::ERROR, "bot command returned an error");
	}
}

class CBotRecipientFilter : public IRecipientFilter
{
public:
	CBotRecipientFilter(const edict_t *pPlayer)
	{
		m_iPlayerSlot = ENTINDEX(pPlayer);
	}

	bool IsReliable() const override { return false; }
	bool IsInitMessage() const override { return false; }

	int	GetRecipientCount() const override { return 1; }
	int	GetRecipientIndex(int slot) const override { return m_iPlayerSlot; }

private:
	int m_iPlayerSlot;
};

class CClientBroadcastRecipientFilter : public IRecipientFilter
{
public:

	CClientBroadcastRecipientFilter() : m_iMaxCount(0) {

		// Initialize m_iPlayerSlot with a default value, e.g., -1
		std::fill(std::begin(m_iPlayerSlot), std::end(m_iPlayerSlot), -1);

		for (int i = 0; i < RCBOT_MAXPLAYERS; ++i) {
			const CClient* client = CClients::get(i);

			if (client->isUsed()) {
				IPlayerInfo* p = playerinfomanager->GetPlayerInfo(client->getPlayer());

				if (p->IsConnected() && !p->IsFakeClient()) {
					m_iPlayerSlot[m_iMaxCount] = i;
					m_iMaxCount++;
				}
			}
		}
	}

	bool IsReliable() const override { return false; }
	bool IsInitMessage() const override { return false; }

	int	GetRecipientCount() const override { return m_iMaxCount; }
	int	GetRecipientIndex(const int slot) const override { return m_iPlayerSlot[slot] + 1; }

private:

	int m_iMaxCount;
	int m_iPlayerSlot[RCBOT_MAXPLAYERS];
};

///////////////
// hud message
///////////////
void RCBotPluginMeta::HudTextMessage(const edict_t* pEntity, const char* szMessage)
{
	int msgid = 0;
	int imsgsize = 0;
	char msgbuf[64];
	bool bOK; //Unused? [APG]RoboCop[CL]

	int hint = -1;
	int say = -1;

	while ((bOK = servergamedll->GetUserMessageInfo(msgid, msgbuf, 63, imsgsize)) == true)
	{
		if (std::strcmp(msgbuf, "HintText") == 0)
			hint = msgid;
		else if (std::strcmp(msgbuf, "SayText") == 0)
			say = msgid;

		msgid++;
	}

	if (msgid == 0)
		return;

	// if (!bOK)
	// return;

	CBotRecipientFilter filter(pEntity);

	bf_write* buf;

	if (hint > 0) {
		buf = engine->UserMessageBegin(&filter, hint);
		buf->WriteString(szMessage);
		engine->MessageEnd();
	}

	if (say > 0) {
		char chatline[128];
		snprintf(chatline, sizeof(chatline), "\x01\x04[RCBot2]\x01 %s\n", szMessage);

		buf = engine->UserMessageBegin(&filter, say);
		buf->WriteString(chatline);
		engine->MessageEnd();
	}
}

//////////////////////////
// chat broadcast message
//////////////////////////
void RCBotPluginMeta::BroadcastTextMessage(const char* szMessage)
{
	int msgid = 0;
	int imsgsize = 0;
	char msgbuf[64];
	bool bOK; //Unused? [APG]RoboCop[CL]

	int hint = -1; //Unused? [APG]RoboCop[CL]
	int say = -1;

	while ((bOK = servergamedll->GetUserMessageInfo(msgid, msgbuf, 63, imsgsize)) == true)
	{
		if (std::strcmp(msgbuf, "HintText") == 0)
			hint = msgid;
		else if (std::strcmp(msgbuf, "SayText") == 0)
			say = msgid;

		msgid++;
	}

	if (msgid == 0)
		return;

	CClientBroadcastRecipientFilter filter; // Allocate on stack
	if (say > 0) {
		char chatline[128];
		snprintf(chatline, sizeof(chatline), "\x01\x04[RCBot2]\x01 %s\n", szMessage);

		bf_write* buf = engine->UserMessageBegin(&filter, say);
		buf->WriteString(chatline);
		engine->MessageEnd();
	}
}

bf_write *RCBotPluginMeta::Hook_UserMessageBegin(IRecipientFilter *pFilter, int iMsgType)
{
	// Resolve the message index once (-2 = looked up and not present, so we stop searching).
	if (m_iSetPlayerLocationMsg == -1)
	{
		m_iSetPlayerLocationMsg = -2;

		int iId = 0;
		char szName[64];
		int iSize = 0;

		while (servergamedll->GetUserMessageInfo(iId, szName, sizeof(szName) - 1, iSize))
		{
			if (std::strcmp(szName, "SetPlayerLocation") == 0)
				m_iSetPlayerLocationMsg = iId;
			else if (std::strcmp(szName, "TextMsg") == 0)
				m_iTextMsg = iId;

			iId++;
		}
	}

	if (iMsgType == m_iSetPlayerLocationMsg && pFilter != nullptr && pFilter->GetRecipientCount() > 0)
	{
		m_iPendingLocationRecipient = pFilter->GetRecipientIndex(0); // player entindex
		m_pPendingLocationBuf = META_RESULT_ORIG_RET(bf_write *);    // buffer FF writes the name into
	}

	// Game refusals (vehicle purchase etc.) reach bots only as TextMsg.
	if (pFilter != nullptr && pFilter->GetRecipientCount() == 1 && iMsgType != m_iSetPlayerLocationMsg)
	{
		const int recipient = pFilter->GetRecipientIndex(0);
		edict_t* edict = engine->PEntityOfEntIndex(recipient);
		IPlayerInfo* info = edict != nullptr ? playerinfomanager->GetPlayerInfo(edict) : nullptr;
		if (info != nullptr && info->IsFakeClient())
		{
			m_iPendingTextRecipient = recipient;
			m_iPendingTextType = iMsgType;
			m_pPendingTextBuf = META_RESULT_ORIG_RET(bf_write *);
		}
	}

	RETURN_META_VALUE(MRES_IGNORED, nullptr);
}

void RCBotPluginMeta::Hook_MessageEnd()
{
	if (m_iPendingTextRecipient > 0 && m_pPendingTextBuf != nullptr)
	{
		// Unknown layout: show the message name and printable bytes.
		const int bytes = m_pPendingTextBuf->GetNumBytesWritten();
		const unsigned char* data = m_pPendingTextBuf->GetData();
		char text[161];
		int n = 0;
		for (int i = 0; i < bytes && n < 160; ++i)
			text[n++] = (data[i] >= 32 && data[i] < 127) ? static_cast<char>(data[i]) : '.';
		text[n] = 0;
		char name[64] = "?";
		int size = 0;
		servergamedll->GetUserMessageInfo(m_iPendingTextType, name, sizeof(name) - 1, size);
		static int s_logged = 0;
		edict_t* edict = engine->PEntityOfEntIndex(m_iPendingTextRecipient);
		if (std::strcmp(name, "VGUIMenu") != 0 && s_logged < 400)
		{
			++s_logged;
			RCBOT_DIAG("[RCBOT2] textmsg userid=%d type=%s bytes=%d msg=%s\n",
				edict != nullptr ? engine->GetPlayerUserId(edict) : -1, name, bytes, text);
		}
	}
	m_iPendingTextRecipient = -1;
	m_pPendingTextBuf = nullptr;

	if (m_iPendingLocationRecipient > 0 && m_pPendingLocationBuf != nullptr)
	{
		bf_read read(m_pPendingLocationBuf->GetData(), m_pPendingLocationBuf->GetNumBytesWritten());

		char szLocation[128];
		read.ReadString(szLocation, sizeof(szLocation));

		const int iSlot = m_iPendingLocationRecipient - 1; // entindex -> 0-based client slot

		if (iSlot >= 0 && iSlot < RCBOT_MAXPLAYERS)
			CClients::get(iSlot)->setLocation(szLocation);
	}

	m_iPendingLocationRecipient = -1;
	m_pPendingLocationBuf = nullptr;

	RETURN_META(MRES_IGNORED);
}

void RCBotPluginMeta::Hook_PlayerRunCmd(CUserCmd *ucmd, IMoveHelper *moveHelper)
{
#ifdef RCBOT_VPROF_ENABLED
	VPROF_BUDGET("RCBotPluginMeta::Hook_PlayerRunCmd", "RCBot2")
#endif // RCBOT_VPROF_ENABLED

	static CBot *pBot;

	CBaseEntity *pPlayer = META_IFACEPTR(CBaseEntity);

	const edict_t *pEdict = servergameents->BaseEntityToEdict(pPlayer);

	pBot = CBots::getBotPointer(pEdict);
	
	if ( pBot && !pBot->isControlledTestOnly() )
	{
		static CBotCmd *cmd;
		
		cmd = pBot->getUserCMD();

		// put the bot's commands into this move frame
		ucmd->buttons = cmd->buttons;
		ucmd->forwardmove = cmd->forwardmove;
		ucmd->impulse = cmd->impulse;
		ucmd->sidemove = cmd->sidemove;
		ucmd->upmove = cmd->upmove;
		ucmd->viewangles = cmd->viewangles;
		ucmd->weaponselect = cmd->weaponselect;
		ucmd->weaponsubtype = cmd->weaponsubtype;
		ucmd->tick_count = cmd->tick_count;
		ucmd->command_number = cmd->command_number;
	}
	RETURN_META(MRES_IGNORED);
}

/** 
 * Something like this is needed to register cvars/CON_COMMANDs.
 */
class BaseAccessor : public IConCommandBaseAccessor
{
public:
	virtual ~BaseAccessor() = default;

	void PrepareRegistration()
	{
		m_bSawRegistration = false;
	}

	bool RegisterConCommandBase(ConCommandBase *pCommandBase) override
	{
		if (pCommandBase != nullptr &&
			std::find(m_Commands.begin(), m_Commands.end(), pCommandBase) == m_Commands.end())
		{
			m_Commands.push_back(pCommandBase);
		}

		if (!m_bReplaying)
			m_bSawRegistration = true;

		/* Always call META_REGCVAR instead of going through the engine. */
		return META_REGCVAR(pCommandBase);
	}

	void ReplayRegistrationIfNeeded()
	{
		/*
		 * SDK2013 consumes its static ConCommandBase list in ConVar_Register()
		 * and does not rebuild that list after ConVar_Unregister().  Metamod
		 * removes the old registrations correctly, but a later Load() therefore
		 * receives no callbacks from the SDK.  Replay the objects observed on
		 * the first registration through the public Metamod registration API.
		 */
		if (m_bSawRegistration || m_Commands.empty())
			return;

		META_LOG(g_PLAPI, "Re-registering %zu RCBot2 console objects after SDK unload",
			m_Commands.size());

		m_bReplaying = true;
		for (ConCommandBase *pCommandBase : m_Commands)
		{
			if (!META_REGCVAR(pCommandBase))
				META_LOG(g_PLAPI, "Failed to re-register RCBot2 console object");
		}
		m_bReplaying = false;
	}

private:
	std::vector<ConCommandBase *> m_Commands;
	bool m_bSawRegistration = false;
	bool m_bReplaying = false;
} s_BaseAccessor;

bool RCBotPluginMeta::Load(PluginId id, ISmmAPI *ismm, char *error, std::size_t maxlen, bool late)
{
	extern MTRand_int32 irand;

	PLUGIN_SAVEVARS()

	GET_V_IFACE_CURRENT(GetEngineFactory, enginetrace, IEngineTrace, INTERFACEVERSION_ENGINETRACE_SERVER)
	GET_V_IFACE_CURRENT(GetEngineFactory, engine, IVEngineServer, INTERFACEVERSION_VENGINESERVER)
	GET_V_IFACE_CURRENT(GetEngineFactory, gameevents, IGameEventManager2, INTERFACEVERSION_GAMEEVENTSMANAGER2)
	GET_V_IFACE_CURRENT(GetEngineFactory, helpers, IServerPluginHelpers, INTERFACEVERSION_ISERVERPLUGINHELPERS)
	GET_V_IFACE_CURRENT(GetEngineFactory, icvar, ICvar, CVAR_INTERFACE_VERSION)
	GET_V_IFACE_CURRENT(GetEngineFactory, staticpropmgr, IStaticPropMgrServer, INTERFACEVERSION_STATICPROPMGR_SERVER)

	GET_V_IFACE_ANY(GetEngineFactory, filesystem, IFileSystem, FILESYSTEM_INTERFACE_VERSION)

	GET_V_IFACE_ANY(GetServerFactory, servergameents, IServerGameEnts, INTERFACEVERSION_SERVERGAMEENTS)
	GET_V_IFACE_ANY(GetServerFactory, server, IServerGameDLL, INTERFACEVERSION_SERVERGAMEDLL)
	GET_V_IFACE_ANY(GetServerFactory, gameclients, IServerGameClients, INTERFACEVERSION_SERVERGAMECLIENTS)
	GET_V_IFACE_ANY(GetServerFactory, playerinfomanager, IPlayerInfoManager, INTERFACEVERSION_PLAYERINFOMANAGER)
	// EntityInfoManager is optional. Empires does not expose it, and a hard
	// GET_V_IFACE_ANY would reject the entire plugin before it can load.
	entityinfomanager = (IEntityInfoManager *)ismm->VInterfaceMatch(
		ismm->GetServerFactory(false), INTERFACEVERSION_ENTITYINFOMANAGER, 0);

	GET_V_IFACE_ANY(GetServerFactory, g_pEffects, IEffects, IEFFECTS_INTERFACE_VERSION)
	GET_V_IFACE_ANY(GetServerFactory, g_pBotManager, IBotManager, INTERFACEVERSION_PLAYERBOTMANAGER)
	GET_V_IFACE_ANY(GetServerFactory, servertools, IServerTools, VSERVERTOOLS_INTERFACE_VERSION)

#ifndef __linux__
	GET_V_IFACE_CURRENT(GetEngineFactory,debugoverlay, IVDebugOverlay, VDEBUG_OVERLAY_INTERFACE_VERSION)
#endif
	GET_V_IFACE_ANY(GetServerFactory, servergamedll, IServerGameDLL, INTERFACEVERSION_SERVERGAMEDLL)
	GET_V_IFACE_ANY(GetServerFactory, gameclients, IServerGameClients, INTERFACEVERSION_SERVERGAMECLIENTS)

	gpGlobals = ismm->GetCGlobals();

	META_LOG(g_PLAPI, "Starting plugin.");

	/* Load the VSP listener.  This is usually needed for IServerPluginHelpers. */
	ismm->AddListener(this, this);
	if ((vsp_callbacks = ismm->GetVSPInfo(nullptr)) == nullptr)
	{
		ismm->EnableVSPListener();
	}

	SH_ADD_HOOK_MEMFUNC(IServerGameDLL, LevelInit, server, this, &RCBotPluginMeta::Hook_LevelInit, true);
	SH_ADD_HOOK_MEMFUNC(IServerGameDLL, ServerActivate, server, this, &RCBotPluginMeta::Hook_ServerActivate, true);
	SH_ADD_HOOK_MEMFUNC(IServerGameDLL, GameFrame, server, this, &RCBotPluginMeta::Hook_GameFrame, true);
	SH_ADD_HOOK_MEMFUNC(IServerGameDLL, LevelShutdown, server, this, &RCBotPluginMeta::Hook_LevelShutdown, false);
	SH_ADD_HOOK_MEMFUNC(IServerGameClients, ClientActive, gameclients, this, &RCBotPluginMeta::Hook_ClientActive, true);
	SH_ADD_HOOK_MEMFUNC(IServerGameClients, ClientDisconnect, gameclients, this, &RCBotPluginMeta::Hook_ClientDisconnect, true);
	SH_ADD_HOOK_MEMFUNC(IServerGameClients, ClientPutInServer, gameclients, this, &RCBotPluginMeta::Hook_ClientPutInServer, true);
	SH_ADD_HOOK_MEMFUNC(IServerGameClients, ClientConnect, gameclients, this, &RCBotPluginMeta::Hook_ClientConnect, false);
	SH_ADD_HOOK_MEMFUNC(IServerGameClients, ClientCommand, gameclients, this, &RCBotPluginMeta::Hook_ClientCommand, false);
	//Hook FireEvent to our function - unstable for TF2? [APG]RoboCop[CL]
	SH_ADD_HOOK_MEMFUNC(IGameEventManager2, FireEvent, gameevents, this, &RCBotPluginMeta::FireGameEvent, false);
	SH_ADD_HOOK_MEMFUNC(IVEngineServer, UserMessageBegin, engine, this, &RCBotPluginMeta::Hook_UserMessageBegin, true);
	SH_ADD_HOOK_MEMFUNC(IVEngineServer, MessageEnd, engine, this, &RCBotPluginMeta::Hook_MessageEnd, false);

#if SOURCE_ENGINE >= SE_ORANGEBOX
	g_pCVar = icvar;
	s_BaseAccessor.PrepareRegistration();
	ConVar_Register(0, &s_BaseAccessor);
	s_BaseAccessor.ReplayRegistrationIfNeeded();
#else
	ConCommandBaseMgr::OneTimeInit(&s_BaseAccessor);
#endif

#if SOURCE_ENGINE!=SE_DARKMESSIAH
	// read loglevel from startup param for early logging
	ConVarRef rcbot_loglevel("rcbot_loglevel");
	rcbot_loglevel.SetValue(CommandLine()->ParmValue("+rcbot_loglevel", rcbot_loglevel.GetInt()));
#endif

	// Read Signatures and Offsets
	CBotGlobals::initModFolder();
	CBotGlobals::readRCBotFolder();

	char filename[512];
	// Load RCBOT2 hook data
	CBotGlobals::buildFileName(filename, "hookinfo", BOT_CONFIG_FOLDER, "ini");

	std::fstream fp(filename, std::fstream::in);

	CRCBotKeyValueList kvl;

	if (fp)
		kvl.parseFile(fp);

	void *gameServerFactory = reinterpret_cast<void*>(ismm->GetServerFactory(false));

	int val;

#if defined(_WIN64) || defined(_WIN32)
	if (kvl.getInt("runplayermove_dods_win", &val))
		rcbot_runplayercmd_dods.SetValue(val);
	if (kvl.getInt("gamerules_win", &val))
		rcbot_gamerules_offset.SetValue(val);
	if (kvl.getInt("runplayermove_synergy_win", &val))
		rcbot_runplayercmd_syn.SetValue(val);
	if (kvl.getInt("getdatadescmap_win", &val))
		rcbot_datamap_offset.SetValue(val);
#else
	if (kvl.getInt("runplayermove_dods_linux", &val))
		rcbot_runplayercmd_dods.SetValue(val);
	if (kvl.getInt("runplayermove_synergy_linux", &val))
		rcbot_runplayercmd_syn.SetValue(val);
	if (kvl.getInt("getdatadescmap_linux", &val))
		rcbot_datamap_offset.SetValue(val);
#endif

	g_pGameRules_Obj = new CGameRulesObject(kvl, gameServerFactory);
	g_pGameRules_Create_Obj = new CCreateGameRulesObject(kvl, gameServerFactory);

	if (fp)
		fp.close();

	if (!CBotGlobals::gameStart())
	{
		snprintf(error, maxlen, "Game mod not recognized (gamedir: \"%s\")", CBotGlobals::modFolder());
		return false;
	}

	CBotMod *pMod = CBotGlobals::getCurrentMod(); // `*pMod` Unused? [APG]RoboCop[CL]

#ifdef OVERRIDE_RUNCMD
	// TODO figure out a more robust gamedata fix instead of vtable
	#if SOURCE_ENGINE == SE_DODS
	SH_MANUALHOOK_RECONFIGURE(MHook_PlayerRunCmd, rcbot_runplayercmd_dods.GetInt(), 0, 0);
	#elif SOURCE_ENGINE == SE_SDK2013
	if(pMod->getModId() == MOD_SYNERGY)
	{
		SH_MANUALHOOK_RECONFIGURE(MHook_PlayerRunCmd, rcbot_runplayercmd_syn.GetInt(), 0, 0);
	}
#endif

#endif

	ENGINE_CALL(LogPrint)("All hooks started!\n");

	//MathLib_Init( 2.2f, 2.2f, 0.0f, 2.0f );
	//ConVar_Register( 0 );
	//InitCVars( interfaceFactory ); // register any cvars we have defined

	std::srand( static_cast<unsigned>(time(nullptr)) );  // initialize the random seed
	MTRand_int32::seed( static_cast<unsigned>(time(nullptr)) );

	// Find the RCBOT2 Path from metamod VDF
	extern IFileSystem* filesystem;
	KeyValues* mainkv = new KeyValues("metamodplugin");

	const char* rcbot2path; //Unused? [APG]RoboCop[CL]
	logger->Log(LogLevel::INFO, "Reading rcbot2 path from VDF...");

	mainkv->LoadFromFile(filesystem, "addons/metamod/rcbot2.vdf", "MOD");
	KeyValues* temp = mainkv->FindKey("Metamod Plugin");

	if (temp)
		rcbot2path = temp->GetString("rcbot2path", "\0");

	mainkv->deleteThis(); //mainkv possible redundant? [APG]RoboCop[CL]
	mainkv = temp; // Memory leak fix [APG]RoboCop[CL]

	//eventListener2 = new CRCBotEventListener();

	// Initialize bot variables
	CBotProfiles::setupProfiles();


	//CBotEvents::setupEvents();
	CWaypointTypes::setup();
	CWaypoints::setupVisibility();

	CBotConfigFile::reset();	
	CBotConfigFile::load();

	CBotMenuList::setupMenus();

	//CRCBotPlugin::ShowLicense();	

	//RandomSeed((unsigned)time(NULL));

	CClassInterface::init();

	RCBOT2_Cvar_setup(g_pCVar);
	RCBotNavMesh::Initialize();

	// Bot Quota Settings
	char bq_line[128];

	int bot_count = 0;
	int human_count = 0;

	for (int& m_iTargetBot : m_iTargetBots)
	{
		m_iTargetBot = 0;
	}

	CBotGlobals::buildFileName(filename, "bot_quota", BOT_CONFIG_FOLDER, "ini");
	fp = std::fstream(filename, std::fstream::in);

	std::memset(bq_line, 0, sizeof(bq_line));

	if (fp) {
		while (fp.getline(bq_line, sizeof(bq_line))) {
			if (bq_line[0] == '#')
				continue;

			for (char& i : bq_line)
			{
				if (i == '\0')
					break;

				if (!isdigit(i))
					i = ' ';
			}

			if (std::sscanf(bq_line, "%d %d", &human_count, &bot_count) == 2) {
				if (human_count < 0 || human_count > RCBOT_MAXPLAYERS) {
					logger->Log(LogLevel::WARN, "Bot Quota - Invalid Human Count %d", human_count);
					continue;
				}

				if (bot_count < 0 || bot_count > RCBOT_MAXPLAYERS) {
					logger->Log(LogLevel::WARN, "Bot Quota - Invalid Bot Count %d", bot_count);
					continue;
				}

				if (human_count < RCBOT_MAXPLAYERS) { // Ensure human_count is within bounds
					m_iTargetBots[human_count] = bot_count;
					logger->Log(LogLevel::INFO, "Bot Quota - Humans: %d, Bots: %d", human_count, bot_count);
				}
			}
		}
	}

	return true;
}

bool RCBotPluginMeta::FireGameEvent(IGameEvent* pevent, bool bDontBroadcast)
{
#ifdef RCBOT_VPROF_ENABLED
	VPROF_BUDGET("RCBotPluginMeta::FireGameEvent", "RCBot2")
#endif // RCBOT_VPROF_ENABLED

	// Skip processing if another plugin already handled/blocked this event - [APG]RoboCop[CL]
	if (META_RESULT_STATUS >= MRES_OVERRIDE)
	{
		RETURN_META_VALUE(MRES_IGNORED, true);
	}

	// Sanity check: ensure event pointer is valid before processing - [APG]RoboCop[CL]
	if (pevent != nullptr)
	{
		const char* eventName = pevent->GetName();
		if (eventName != nullptr &&
			(std::strcmp(eventName, "point_captured") == 0 ||
			 std::strcmp(eventName, "point_neutral") == 0))
		{
			EmpiresAnalysis::RecordNativeCaptureEvent(eventName,
				pevent->GetBool("team", false), pevent->GetInt("location", -1));
		}
		else if (eventName != nullptr && logger != nullptr &&
			std::strcmp(eventName, "vehicle_purchase") == 0)
		{
			logger->Log(LogLevel::INFO,
				"Empires gameplay vehicle_purchase userid=%d recustomized=%d cost=%d chassis=%d builtfromwages=%d",
				pevent->GetInt("userid", -1), pevent->GetInt("recustomized", -1),
				pevent->GetInt("cost", -1), pevent->GetInt("chassis", -1),
				pevent->GetInt("builtfromwages", -1));
		}
		else if (eventName != nullptr && logger != nullptr &&
			std::strcmp(eventName, "vehicle_enter") == 0)
		{
			logger->Log(LogLevel::INFO,
				"Empires gameplay vehicle_enter userid=%d vehicleid=%d seat=%d",
				pevent->GetInt("userid", -1), pevent->GetInt("vehicleid", -1),
				pevent->GetInt("seat", -1));
		}
		else if (eventName != nullptr && logger != nullptr &&
			std::strcmp(eventName, "vehicle_exit") == 0)
		{
			logger->Log(LogLevel::INFO,
				"Empires gameplay vehicle_exit userid=%d vehicleid=%d seatnumber=%d",
				pevent->GetInt("userid", -1), pevent->GetInt("vehicleid", -1),
				pevent->GetInt("seatnumber", -1));
		}
		else if (eventName != nullptr && std::strcmp(eventName, "voice_command") == 0)
		{
			logger->Log(LogLevel::INFO,
				"Empires gameplay voice_command event team=%d command=%d sub=%d userid=%d",
				pevent->GetInt("team", -1), pevent->GetInt("command", -1),
				pevent->GetInt("sub", -1), pevent->GetInt("userid", -1));
		}

		CBotEvents::executeEvent(pevent, TYPE_IGAMEEVENT);
	}

	RETURN_META_VALUE(MRES_IGNORED, true);
}

bool RCBotPluginMeta::Unload(char *error, std::size_t maxlen)
{
#ifdef SM_EXT
	SM_UnloadExtension();
#endif
	
	//CBots::kickRandomBot(RCBOT_MAXPLAYERS); //breaks the bot quota system? [APG]RoboCop[CL]
	
	SH_REMOVE_HOOK_MEMFUNC(IServerGameDLL, LevelInit, server, this, &RCBotPluginMeta::Hook_LevelInit, true);
	SH_REMOVE_HOOK_MEMFUNC(IServerGameDLL, ServerActivate, server, this, &RCBotPluginMeta::Hook_ServerActivate, true);
	SH_REMOVE_HOOK_MEMFUNC(IServerGameDLL, GameFrame, server, this, &RCBotPluginMeta::Hook_GameFrame, true);
	SH_REMOVE_HOOK_MEMFUNC(IServerGameDLL, LevelShutdown, server, this, &RCBotPluginMeta::Hook_LevelShutdown, false);
	SH_REMOVE_HOOK_MEMFUNC(IServerGameClients, ClientActive, gameclients, this, &RCBotPluginMeta::Hook_ClientActive, true);
	SH_REMOVE_HOOK_MEMFUNC(IServerGameClients, ClientDisconnect, gameclients, this, &RCBotPluginMeta::Hook_ClientDisconnect, true);
	SH_REMOVE_HOOK_MEMFUNC(IServerGameClients, ClientPutInServer, gameclients, this, &RCBotPluginMeta::Hook_ClientPutInServer, true);
	SH_REMOVE_HOOK_MEMFUNC(IServerGameClients, ClientConnect, gameclients, this, &RCBotPluginMeta::Hook_ClientConnect, false);
	SH_REMOVE_HOOK_MEMFUNC(IServerGameClients, ClientCommand, gameclients, this, &RCBotPluginMeta::Hook_ClientCommand, false);
	// Unhook the FireEvent hook to avoid potential crashes during plugin unload and for dod_broadcast_audio? [APG]RoboCop[CL]
	SH_REMOVE_HOOK_MEMFUNC(IGameEventManager2, FireEvent, gameevents, this, &RCBotPluginMeta::FireGameEvent, false);
	SH_REMOVE_HOOK_MEMFUNC(IVEngineServer, UserMessageBegin, engine, this, &RCBotPluginMeta::Hook_UserMessageBegin, true);
	SH_REMOVE_HOOK_MEMFUNC(IVEngineServer, MessageEnd, engine, this, &RCBotPluginMeta::Hook_MessageEnd, false);

	//SH_REMOVE_MANUALHOOK(MHook_PlayerRunCmd, player_vtable, SH_STATIC(Hook_Function2), false);

	// if another instance is running dont run through this
	//if ( !bInitialised )
	//	return;
	
	RCBotNavMeshProvider::LevelShutdown();
	RCBotLocalExplorer::LevelShutdown();
	CBots::freeAllMemory();
	CStrings::freeAllMemory();
	CBotMods::freeMemory();
	CAccessClients::freeMemory();
	CBotEvents::freeMemory();
	CWaypoints::freeMemory();
	CWaypointTypes::freeMemory();
	CBotProfiles::deleteProfiles();
	CWeapons::freeMemory();
	CBotMenuList::freeMemory();
	RCBotNavMesh::Shutdown();
	//unloadSignatures();

	//UnhookPlayerRunCommand();
	//UnhookGiveNamedItem();

	//ConVar_Unregister();

	//if ( gameevents )
	//	gameevents->RemoveListener(this);

	ConVar_Unregister( );

	return true;
}

void RCBotPluginMeta::OnVSPListening(IServerPluginCallbacks *iface)
{
	vsp_callbacks = iface;
}

void RCBotPluginMeta::Hook_ServerActivate(edict_t *pEdictList, const int edictCount, const int clientMax)
{
	META_LOG(g_PLAPI, "ServerActivate() called: edictCount = %d, clientMax = %d", edictCount, clientMax);

	CAccessClients::load();

	CBotGlobals::setClientMax(clientMax);
}

void RCBotPluginMeta::AllPluginsLoaded()
{
	/* This is where we'd do stuff that relies on the mod or other plugins 
	 * being initialized (for example, cvars added and events registered).
	 */
#ifdef SM_EXT
	BindToSourcemod();
#endif
}

#ifdef SM_EXT
void* RCBotPluginMeta::OnMetamodQuery(const char* iface, int *ret) {
	if (std::strcmp(iface, SOURCEMOD_NOTICE_EXTENSIONS) == 0) {
		BindToSourcemod();
	}
	
	if (ret != nullptr) {
		*ret = IFACE_OK;
	}
	
	return nullptr;
}
#endif

void RCBotPluginMeta::Hook_ClientActive(edict_t *pEntity, const bool bLoadGame)
{
	META_LOG(g_PLAPI, "Hook_ClientActive(%d, %d)", IndexOfEdict(pEntity), bLoadGame);

	CClients::clientActive(pEntity);
}

#if SOURCE_ENGINE >= SE_ORANGEBOX
void RCBotPluginMeta::Hook_ClientCommand(edict_t *pEntity, const CCommand &args)
#else
void RCBotPluginMeta::Hook_ClientCommand(edict_t *pEntity)
#endif
{
#ifdef RCBOT_VPROF_ENABLED
	VPROF_BUDGET("RCBotPluginMeta::Hook_ClientCommand", "RCBot2")
#endif // RCBOT_VPROF_ENABLED

	static CBotMod *pMod = nullptr;

#if SOURCE_ENGINE <= SE_DARKMESSIAH
	CCommand args;
#endif

	const char *pcmd = args.Arg(0);

	if (!pEntity || pEntity->IsFree())
	{
		return;
	}

	if (pcmd && std::strcmp(pcmd, "emp_menu_command") == 0 && args.ArgC() >= 2 &&
		std::strcmp(args.Arg(1), "29") == 0 && CBotGlobals::getCurrentMod() &&
		CBotGlobals::getCurrentMod()->getBotType() == BOTTYPE_EMPIRES)
		EmpiresRecordInfantryAmmoCall(pEntity);

	CClient *pClient = CClients::get(pEntity);

	// EngiBot purchase from the client console ("buildengibot", also for a key bind): works even
	// where a chat plugin hides or swallows "/buildengibot". Queued like the chat command.
	if ( pcmd != nullptr && std::strcmp(pcmd, "buildengibot") == 0 )
	{
		EmpiresHandleEngibotChat(pEntity, "/buildengibot");
		RETURN_META(MRES_SUPERCEDE);
	}

	// is bot command?
	if ( CBotGlobals::m_pCommands->isCommand(pcmd) )
	{		
		// create shifted command list
		BotCommandArgs argList;
		for (unsigned i = 1; i <= static_cast<unsigned>(args.ArgC()); i++) {
			argList.emplace_back(args.Arg(static_cast<int>(i)));
		}
		const eBotCommandResult iResult = CBotGlobals::m_pCommands->execute(pClient, argList);

		if ( iResult == COMMAND_ACCESSED )
		{
			// ok
		}
		else if ( iResult == COMMAND_REQUIRE_ACCESS )
		{
			CBotGlobals::botMessage(pEntity,0,"You do not have access to this command");
		}
		else if ( iResult == COMMAND_NOT_FOUND )
		{
			CBotGlobals::botMessage(pEntity,0,"bot command not found");	
		}
		else if ( iResult == COMMAND_ERROR )
		{
			CBotGlobals::botMessage(pEntity,0,"bot command returned an error");	
		}

		RETURN_META(MRES_SUPERCEDE);
	}
	if ( std::strncmp(pcmd,"menuselect",10) == 0 ) // menu command
	{
		if ( pClient->isUsingMenu() )
		{
			int iCommand = std::atoi(args.Arg(1));

			// format is 1.2.3.4.5.6.7.8.9.0
			if ( iCommand == 0 )
				iCommand = 9;
			else
				iCommand --;

			pClient->getCurrentMenu()->selectedMenu(pClient,iCommand);
		}
	}

	// command capturing
	pMod = CBotGlobals::getCurrentMod();

	// capture some client commands e.g. voice commands
	pMod->clientCommand(pEntity,args.ArgC(),pcmd,args.Arg(1),args.Arg(2));

	RETURN_META(MRES_IGNORED); 
}

bool RCBotPluginMeta::Hook_ClientConnect(edict_t *pEntity,
									const char *pszName,
									const char *pszAddress,
									char *reject,
									int maxrejectlen)
{
	META_LOG(g_PLAPI, R"(Hook_ClientConnect(%d, "%s", "%s"))", IndexOfEdict(pEntity), pszName, pszAddress);

	CClients::init(pEntity);

	return true;
}

void RCBotPluginMeta::Hook_ClientPutInServer(edict_t *pEntity, char const* playername)
{
	CBaseEntity *pEnt = servergameents->EdictToBaseEntity(pEntity); //`*pEnt` Unused? [APG]RoboCop[CL]
	constexpr bool is_Rcbot = false; //`is_Rcbot` Unused? [APG]RoboCop[CL]

	if ( CClient *pClient = CClients::clientConnected(pEntity) )
	{
		if ( !engine->IsDedicatedServer() )
		{
			if ( CClients::noListenServerClient() )
			{
				// give listenserver client all access to bot commands
				CClients::setListenServerClient(pClient);
				pClient->setAccessLevel(CMD_ACCESS_ALL);
				pClient->resetMenuCommands();
			}
		}
	}

	CBotMod *pMod = CBotGlobals::getCurrentMod();

	pMod->playerSpawned(pEntity);

#ifdef OVERRIDE_RUNCMD
	// Don't add the PlayerRunCmd vtable hook for FF or Empires - the vtable
	// offset is not configured for either mod and hooking at offset 0 would
	// hook the wrong function. These bots use
	// m_pController->RunPlayerMove() directly instead.
	if ( pEnt && !CBotGlobals::isMod(MOD_FF) &&
		CBotGlobals::getCurrentMod()->getBotType() != BOTTYPE_EMPIRES )
	{
		SH_ADD_MANUALHOOK_MEMFUNC(MHook_PlayerRunCmd, pEnt, this, &RCBotPluginMeta::Hook_PlayerRunCmd, false);
	}
#endif
}

void RCBotPluginMeta::Hook_ClientDisconnect(edict_t *pEntity)
{
	CBaseEntity *pEnt = servergameents->EdictToBaseEntity(pEntity); //`*pEnt` Unused? [APG]RoboCop[CL]

#ifdef OVERRIDE_RUNCMD
	if ( pEnt && !CBotGlobals::isMod(MOD_FF) &&
		CBotGlobals::getCurrentMod()->getBotType() != BOTTYPE_EMPIRES )
	{
		SH_REMOVE_MANUALHOOK_MEMFUNC(MHook_PlayerRunCmd, pEnt, this, &RCBotPluginMeta::Hook_PlayerRunCmd, false);
	}
#endif

	EmpiresLawlietOnClientDisconnect(IndexOfEdict(pEntity));
	CClients::clientDisconnected(pEntity);

	META_LOG(g_PLAPI, "Hook_ClientDisconnect(%d)", IndexOfEdict(pEntity));
}

void RCBotPluginMeta::Hook_GameFrame(const bool simulating)
{
#ifdef RCBOT_VPROF_ENABLED
	VPROF_BUDGET("RCBotPluginMeta::Hook_GameFrame", "RCBot2")
#endif // RCBOT_VPROF_ENABLED

	// Low-noise runtime evidence for the native Empires harness.  The engine
	// passes false while hibernating; logging only state transitions lets the
	// harness distinguish a real normal GameFrame path from a keepalive client
	// that is merely listed as active, without changing bot behavior.
	static int lastSimulating = -1;
	const int currentSimulating = simulating ? 1 : 0;
	if (currentSimulating != lastSimulating)
	{
		META_LOG(g_PLAPI, "[RCBOT2] GameFrame simulating=%d map_running=%d",
			currentSimulating, CBotGlobals::IsMapRunning() ? 1 : 0);
		lastSimulating = currentSimulating;
	}

	if (simulating && CBotGlobals::IsMapRunning())
	{
		RCBotNavMesh::Frame(true);
		RCBotNavMeshPoc::Frame();
	}

	/**
	 * simulating:
	 * ***********
	 * true  | game is ticking
	 * false | game is not ticking
	 */

	if ( simulating && CBotGlobals::IsMapRunning() )
	{
		static CBotMod *currentmod;

		CBots::botThink();
		// The alternative provider writes its next target after normal task
		// execution. CBot::doMove()/RunPlayerMove consumes that target on the
		// next regular bot frame, while legacy writers remain ownership-guarded.
		RCBotNavMeshProvider::Frame();
		// The local explorer is an explicit test-only owner. It writes through
		// the same target seam after botThink(), so normal doMove()/RunPlayerMove
		// remains the only locomotion controller.
		RCBotLocalExplorer::Frame();
		CClients::clientThink();

		if ( CWaypoints::getVisiblity()->needToWorkVisibility() )
		{
			CWaypoints::getVisiblity()->workVisibility();
		}

		// Profiling
#ifdef _DEBUG
		if ( CClients::clientsDebugging(BOT_DEBUG_PROFILE) )
		{
			CProfileTimers::updateAndDisplay();
		}
#endif

		// Config Commands
		CBotConfigFile::doNextCommand();
		currentmod = CBotGlobals::getCurrentMod();

		currentmod->modFrame();

		if (g_bEmpiresDeferredCommanderBot &&
			currentmod != nullptr && currentmod->getBotType() == BOTTYPE_EMPIRES &&
			CBots::getFirstNormalBot() == nullptr)
		{
			edict_t* commander = findEmpiresCommanderInterface();
			if (commander != nullptr)
			{
				const int commanderUserId = engine->GetPlayerUserId(commander);
				if (CBots::createBot("1", "2", "CommanderMoveNF"))
				{
					g_bEmpiresDeferredCommanderBot = false;
					logger->Log(LogLevel::INFO,
						"Empires deferred Commander bot spawn triggered commander_userid=%d trigger=m_bInCommanderInterface=1 (F2) class=1 team=2",
						commanderUserId);
				}
			}
		}

		EmpiresEngibotFrame();

		// Bot Quota
		if (rcbot_bot_quota_interval.GetFloat() > 0.0) {
			BotQuotaCheck();
		}
	}
	else if (!simulating && CBotGlobals::IsMapRunning() && CBots::hasControlledTestOnlyBot())
	{
		// Native Empires hibernates with zero human clients and reports
		// simulating=false. Keep the controlled bootstrap state machine
		// observable without running normal AI, movement, client think or quota.
		CBots::observeBootstrapOnly();
	}
}

void RCBotPluginMeta::BotQuotaCheck() {
	// this is configured with config/bot_quota.ini
	if (rcbot_bot_quota_interval.GetFloat() <= 0.0) {
		return;
	}

	// Keep the controlled bootstrap lifecycle isolated from automatic quota
	// reconciliation. Normal quota behavior resumes after it is removed.
	if (CBots::automaticBotsSuppressed()) {
		return;
	}

	if (m_fBotQuotaTimer < 1.0f) {
		m_fBotQuotaTimer = engine->Time() + 10.0f; // Sleep 10 seconds
	}

	if (m_fBotQuotaTimer < engine->Time() - rcbot_bot_quota_interval.GetFloat()) {
		m_fBotQuotaTimer = engine->Time();

		// Change Notification
		bool notify = false;

		// Current Bot Count
		int bot_count = 0;
		int human_count = 0;

		// Count Players
		for (int i = 0; i < RCBOT_MAXPLAYERS; ++i) {
			const CClient* client = CClients::get(i);
			const CBot* bot = CBots::get(i);

			if (bot != nullptr && bot->getEdict() != nullptr && bot->inUse() && !bot->isControlledTestOnly()) {
				edict_t* pEdict = bot->getEdict();
				if (pEdict->IsFree())
					continue;

				IPlayerInfo *p = playerinfomanager->GetPlayerInfo(pEdict);

				// EngiBots are bought by a commander on top of the quota.
				if (p != nullptr && p->IsConnected() && p->IsPlayer() && p->IsFakeClient() && !p->IsHLTV() &&
					!EmpiresIsEngibot(pEdict)) {
					bot_count++;
				}
			}

			if (client->getPlayer() != nullptr && client->isUsed()) {
				edict_t* pEdict = client->getPlayer();
				if (pEdict->IsFree())
					continue;

				IPlayerInfo* p = playerinfomanager->GetPlayerInfo(pEdict);

				if (p != nullptr && p->IsConnected() && p->IsPlayer() && !p->IsFakeClient() && !p->IsHLTV()) {
					if (rcbot_ignore_spectators.GetBool()) {
						if ( CClassInterface::getTeam(client->getPlayer()) >= 2 )
							human_count++;
					} else
						human_count++;
				}
			}
		}

		if (human_count >= RCBOT_MAXPLAYERS) {
			human_count = 0;
		}

		// Get Bot Quota
		int bot_target = m_iTargetBots[human_count];
		const int max_bots = CBots::getMaxBots();
		const int min_bots = CBots::getMinBots();

		// Use min and max as ceiling and floor
		if ((max_bots > -1) && (bot_target > max_bots)) {
			bot_target = max_bots;
		} else if ((min_bots > -1) && (bot_target < min_bots)) {
			bot_target = min_bots;
		}

		// Change Bot Quota
		// In MVM mode, don't kick RCBots via quota - TF2 manages BLU team slots
		const bool bIsMVM = CTeamFortress2Mod::isMapType(TF_MAP_MVM);

		if (bot_count > bot_target && !bIsMVM) {
			if (rcbot_nonrandom_kicking.GetBool()) {
				CBots::kickChosenBot(static_cast<unsigned>(bot_count - bot_target));
			} else {
				CBots::kickRandomBot(static_cast<unsigned>(bot_count - bot_target));
			}
			notify = true;
		}
		else if ((bot_target > bot_count) && ( CBots::getAddKickBotTime() < engine->Time() )) {
			const int bot_diff = bot_target - bot_count;

			for (int i = 0; i < bot_diff; ++i) {
				CBots::createBot("", "", "");
				if (rcbot_addbottime.GetFloat() > 0.0)
					break;
			}

			notify = true;
		}

		if (notify) {
			char chatmsg[128];
			snprintf(chatmsg, sizeof(chatmsg), "[Bot Quota] Humans: %d, Bots: %d", human_count, bot_target);
			logger->Log(LogLevel::INFO, chatmsg);
			// RCBotPluginMeta::BroadcastTextMessage(chatmsg);
		}
	}
}

bool RCBotPluginMeta::Hook_LevelInit(const char *pMapName,
								char const *pMapEntities,
								char const *pOldLevel,
								char const *pLandmarkName,
								bool loadGame,
								bool background)
{
	META_LOG(g_PLAPI, "Hook_LevelInit(%s)", pMapName);
	RCBotNavMesh::LevelInit();

		//CClients::initall();
	// Must set this
	CBotGlobals::setMapName(pMapName);

	logger->Log(LogLevel::INFO, "Level \"%s\" has been loaded", pMapName);

	CWaypoints::precacheWaypointTexture();

	CWaypointDistances::reset();

	CProfileTimers::reset();

	CWaypoints::init();
	CWaypoints::load();

	CBotGlobals::setMapRunning(true);
	CBotConfigFile::reset();
	
	if ( mp_teamplay.IsValid() )
		CBotGlobals::setTeamplay(mp_teamplay.GetBool());
	else
		CBotGlobals::setTeamplay(false);

	CBotEvents::setupEvents();

	CBots::mapInit();

	if ( CBotMod *pMod = CBotGlobals::getCurrentMod() )
		pMod->mapInit();

	CBotSquads::FreeMemory();

	CClients::setListenServerClient(nullptr);

	// Setup game rules
	extern void **g_pGameRules; //Unused? [APG]RoboCop[CL]

	if (g_pGameRules_Obj && g_pGameRules_Obj->found())
	{
		g_pGameRules = g_pGameRules_Obj->getGameRules();
	}
	else if (g_pGameRules_Create_Obj && g_pGameRules_Create_Obj->found())
	{
		g_pGameRules = g_pGameRules_Create_Obj->getGameRules();
	}

	return true;
}

void RCBotPluginMeta::Hook_LevelShutdown()
{
	META_LOG(g_PLAPI, "Hook_LevelShutdown()");
	RCBotNavMeshProvider::LevelShutdown();
	RCBotLocalExplorer::LevelShutdown();
	RCBotNavMesh::LevelShutdown();
	RCBotNavMeshPoc::LevelShutdown();
	g_bEmpiresDeferredCommanderBot = false;
	CBots::setEmpiresBootstrapTestMode(false);

	CClients::initall();
	CWaypointDistances::save();

	CBots::freeMapMemory();	
	CWaypoints::init();

	CBotGlobals::setMapRunning(false);
	CBotEvents::freeMemory();
}

bool RCBotPluginMeta::Pause(char *error, std::size_t maxlen)
{
	return true;
}

bool RCBotPluginMeta::Unpause(char *error, std::size_t maxlen)
{
	return true;
}

const char *RCBotPluginMeta::GetLicense()
{
	return "GPL General Public License";
}

const char *RCBotPluginMeta::GetVersion()
{
	return build_info::short_version;
}

const char *RCBotPluginMeta::GetDate()
{
	return build_info::date;
}

const char *RCBotPluginMeta::GetLogTag()
{
	return "RCBOT2";
}

const char *RCBotPluginMeta::GetAuthor()
{
	return build_info::authors;
}

const char *RCBotPluginMeta::GetDescription()
{
	return "Bot for HL2DM, TF2 and DOD:S";
}

const char *RCBotPluginMeta::GetName()
{
	return "RCBot2";
}

const char *RCBotPluginMeta::GetURL()
{
	return build_info::url;
}

#ifdef SM_EXT
void RCBotPluginMeta::BindToSourcemod()
{
	char error[256];
	if (!SM_LoadExtension(error, sizeof(error))) {
		char message[512];
		snprintf(message, sizeof(message), "Could not load as a SourceMod extension: %s\n", error);
		engine->LogPrint(message);
	}
}
#endif
